import { Injectable, Logger } from '@nestjs/common';
import axios from 'axios';

// ============================================================
//  KIỂU DỮ LIỆU
// ============================================================

/** Chế độ điều khiển bơm — khớp với enum trong firmware ESP32 */
export type PumpMode = 0 | 1 | 2;
// 0 = AUTO       — tự động theo độ ẩm
// 1 = MANUAL_ON  — bật thủ công
// 2 = MANUAL_OFF — tắt thủ công

export interface SensorStatus {
  moisture: number | null;   // % độ ẩm đất (Field 1)
  pumpRunning: boolean;      // trạng thái bơm   (Field 2)
  pumpMode: PumpMode;        // chế độ điều khiển (Field 3)
  updatedAt: string | null;  // thời gian đo gần nhất (ISO 8601)
  error?: string;            // thông báo lỗi nếu có
}

export interface CommandResult {
  success: boolean;
  message: string;
}

// ============================================================
//  SERVICE
// ============================================================
@Injectable()
export class AppService {
  private readonly logger = new Logger(AppService.name);

  private get channelId()   { return process.env.CHANNEL_ID   || ''; }
  private get readApiKey()  { return process.env.READ_API_KEY  || ''; }
  private get writeApiKey() { return process.env.WRITE_API_KEY || ''; }

  // ----------------------------------------------------------
  //  Đọc trạng thái mới nhất từ ThingSpeak
  // ----------------------------------------------------------
  async getStatus(): Promise<SensorStatus> {
    try {
      const url = `https://api.thingspeak.com/channels/${this.channelId}/feeds/last.json`;
      const { data } = await axios.get(url, {
        params: { api_key: this.readApiKey },
        timeout: 8000,
      });

      const moisture = data.field1 != null ? parseFloat(data.field1) : null;
      const pumpRunning = parseInt(data.field2 ?? '0') === 1;
      const pumpMode = (parseInt(data.field3 ?? '0') as PumpMode);

      return {
        moisture: isNaN(moisture) ? null : moisture,
        pumpRunning,
        pumpMode,
        updatedAt: data.created_at ?? null,
      };
    } catch (err) {
      this.logger.error(`Đọc ThingSpeak thất bại: ${err.message}`);
      return {
        moisture: null,
        pumpRunning: false,
        pumpMode: 0,
        updatedAt: null,
        error: 'Không thể kết nối ThingSpeak. Kiểm tra CHANNEL_ID và API Key.',
      };
    }
  }

  // ----------------------------------------------------------
  //  Ghi chế độ điều khiển bơm lên ThingSpeak (Field 3)
  // ----------------------------------------------------------
  async setPumpMode(mode: PumpMode): Promise<CommandResult> {
    const modeLabel = ['Tự động', 'Bật thủ công', 'Tắt thủ công'][mode];

    try {
      const { data } = await axios.get('https://api.thingspeak.com/update', {
        params: {
          api_key: this.writeApiKey,
          field3: mode,
        },
        timeout: 8000,
      });

      // ThingSpeak trả về entry_id (> 0) khi thành công, 0 khi thất bại
      if (data === 0 || data === '0') {
        return { success: false, message: 'ThingSpeak từ chối ghi. Kiểm tra Write API Key hoặc chờ 15 giây giữa các lần ghi.' };
      }

      this.logger.log(`Đặt chế độ bơm → ${modeLabel} (entry #${data})`);
      return { success: true, message: `Đã đặt chế độ: ${modeLabel}` };
    } catch (err) {
      this.logger.error(`Ghi ThingSpeak thất bại: ${err.message}`);
      return { success: false, message: `Lỗi kết nối: ${err.message}` };
    }
  }
}
