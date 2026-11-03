import { Injectable, Logger } from '@nestjs/common';
import axios from 'axios';

// ============================================================
//  KIỂU DỮ LIỆU
// ============================================================

/** Chế độ điều khiển bơm — khớp với enum trong firmware ESP32 */
export type PumpMode = 0 | 1 | 2;
// 0 = AUTO       — tự động theo độ ẩm
// 1 = MANUAL_ON  — bật thủ công (tối đa 5 phút, sau đó ESP32 tự về AUTO)
// 2 = MANUAL_OFF — tắt thủ công

/** Mã lỗi ESP32 gửi lên Field 4 */
export type ErrorCode = 0 | 1 | 2;
// 0 = bình thường
// 1 = cảm biến lỗi (ADC ngoài dải hợp lệ)
// 2 = bơm chạy mà độ ẩm không tăng (hết nước / tắc ống)

/** Chuỗi lệnh TalkBack tương ứng từng chế độ — khớp với readPumpCommand() */
const TALKBACK_COMMANDS: Record<PumpMode, string> = { 0: 'AUTO', 1: 'ON', 2: 'OFF' };

export interface SensorStatus {
  moisture: number | null;   // % độ ẩm đất (Field 1) — null nếu cảm biến lỗi
  pumpRunning: boolean;      // trạng thái bơm   (Field 2)
  pumpMode: PumpMode;        // chế độ hiện tại do ESP32 báo (Field 3)
  errorCode: ErrorCode;      // mã lỗi (Field 4)
  updatedAt: string | null;  // thời gian đo gần nhất (ISO 8601)
  error?: string;            // thông báo lỗi kết nối nếu có
}

export interface HistoryPoint {
  time: string;              // ISO 8601
  moisture: number | null;
  pump: 0 | 1;
  mode: PumpMode;
  errorCode: ErrorCode;
}

export interface CommandResult {
  success: boolean;
  message: string;
}

interface ThingSpeakFeed {
  created_at?: string;
  field1?: string | null;
  field2?: string | null;
  field3?: string | null;
  field4?: string | null;
}

// ============================================================
//  HELPERS
// ============================================================
function toNumber(v: unknown): number | null {
  if (v === null || v === undefined || v === '') return null;
  const n = parseFloat(String(v));
  return Number.isFinite(n) ? n : null;
}

function toEnum<T extends number>(v: unknown, allowed: readonly T[], fallback: T): T {
  const n = toNumber(v);
  return n !== null && (allowed as readonly number[]).includes(n) ? (n as T) : fallback;
}

// ============================================================
//  SERVICE
// ============================================================
@Injectable()
export class AppService {
  private readonly logger = new Logger(AppService.name);

  private get channelId()      { return process.env.CHANNEL_ID       || ''; }
  private get readApiKey()     { return process.env.READ_API_KEY     || ''; }
  private get talkbackId()     { return process.env.TALKBACK_ID      || ''; }
  private get talkbackApiKey() { return process.env.TALKBACK_API_KEY || ''; }

  // ----------------------------------------------------------
  //  Đọc trạng thái mới nhất từ ThingSpeak
  //  (chỉ ESP32 ghi vào channel → entry cuối luôn đủ field2..4)
  // ----------------------------------------------------------
  async getStatus(): Promise<SensorStatus> {
    try {
      const url = `https://api.thingspeak.com/channels/${this.channelId}/feeds/last.json`;
      const { data } = await axios.get(url, {
        params: { api_key: this.readApiKey },
        timeout: 8000,
      });

      // Channel chưa có dữ liệu → ThingSpeak trả về -1
      if (!data || typeof data !== 'object') {
        return { moisture: null, pumpRunning: false, pumpMode: 0, errorCode: 0, updatedAt: null };
      }

      const feed = data as ThingSpeakFeed;
      return {
        moisture: toNumber(feed.field1),
        pumpRunning: toNumber(feed.field2) === 1,
        pumpMode: toEnum<PumpMode>(feed.field3, [0, 1, 2], 0),
        errorCode: toEnum<ErrorCode>(feed.field4, [0, 1, 2], 0),
        updatedAt: feed.created_at ?? null,
      };
    } catch (err) {
      this.logger.error(`Đọc ThingSpeak thất bại: ${err.message}`);
      return {
        moisture: null,
        pumpRunning: false,
        pumpMode: 0,
        errorCode: 0,
        updatedAt: null,
        error: 'Không thể kết nối ThingSpeak. Kiểm tra CHANNEL_ID và READ_API_KEY.',
      };
    }
  }

  // ----------------------------------------------------------
  //  Lịch sử N điểm gần nhất (cho biểu đồ)
  // ----------------------------------------------------------
  async getHistory(results: number): Promise<HistoryPoint[]> {
    const url = `https://api.thingspeak.com/channels/${this.channelId}/feeds.json`;
    const { data } = await axios.get(url, {
      params: { api_key: this.readApiKey, results },
      timeout: 10000,
    });

    const feeds: ThingSpeakFeed[] = Array.isArray(data?.feeds) ? data.feeds : [];
    return feeds
      .filter((f) => !!f.created_at)
      .map((f) => ({
        time: f.created_at as string,
        moisture: toNumber(f.field1),
        pump: toNumber(f.field2) === 1 ? 1 : 0,
        mode: toEnum<PumpMode>(f.field3, [0, 1, 2], 0),
        errorCode: toEnum<ErrorCode>(f.field4, [0, 1, 2], 0),
      }));
  }

  // ----------------------------------------------------------
  //  Gửi lệnh đổi chế độ vào hàng đợi ThingSpeak TalkBack.
  //  ESP32 lấy lệnh bằng commands/execute (lấy + xóa khỏi hàng đợi),
  //  nên dashboard không còn ghi vào channel → không đè dữ liệu của ESP32
  //  và không đụng giới hạn 15 s/lần ghi.
  // ----------------------------------------------------------
  async setPumpMode(mode: PumpMode): Promise<CommandResult> {
    const modeLabel = ['Tự động', 'Bật thủ công', 'Tắt thủ công'][mode];

    if (!this.talkbackId || !this.talkbackApiKey) {
      return { success: false, message: 'Chưa cấu hình TALKBACK_ID / TALKBACK_API_KEY trong .env' };
    }

    try {
      const url = `https://api.thingspeak.com/talkbacks/${this.talkbackId}/commands.json`;
      const body = new URLSearchParams({
        api_key: this.talkbackApiKey,
        command_string: TALKBACK_COMMANDS[mode],
      });
      const { data } = await axios.post(url, body, { timeout: 8000 });

      this.logger.log(`Đưa lệnh vào TalkBack → ${TALKBACK_COMMANDS[mode]} (command #${data?.id ?? '?'})`);
      return { success: true, message: `Đã gửi lệnh: ${modeLabel} — ESP32 sẽ nhận trong vài giây` };
    } catch (err) {
      const status = err.response?.status;
      this.logger.error(`Gửi lệnh TalkBack thất bại: ${status ?? ''} ${err.message}`);
      if (status === 400 || status === 401 || status === 404) {
        return { success: false, message: 'TalkBack từ chối lệnh. Kiểm tra TALKBACK_ID và TALKBACK_API_KEY.' };
      }
      return { success: false, message: `Lỗi kết nối: ${err.message}` };
    }
  }
}
