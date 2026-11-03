import {
  Controller,
  Get,
  Post,
  Body,
  HttpCode,
  HttpStatus,
  Query,
  BadRequestException,
  BadGatewayException,
} from '@nestjs/common';
import { AppService, PumpMode } from './app.service';

// DTO cho request đổi chế độ bơm
interface SetModeDto {
  mode: PumpMode; // 0 | 1 | 2
}

@Controller('api')
export class AppController {
  constructor(private readonly appService: AppService) {}

  // ----------------------------------------------------------
  //  GET /api/status — đọc trạng thái hiện tại
  // ----------------------------------------------------------
  @Get('status')
  getStatus() {
    return this.appService.getStatus();
  }

  // ----------------------------------------------------------
  //  GET /api/history?results=N — N điểm gần nhất (1..8000, mặc định 100)
  // ----------------------------------------------------------
  @Get('history')
  async getHistory(@Query('results') results?: string) {
    const n = Math.min(8000, Math.max(1, parseInt(results ?? '100', 10) || 100));
    try {
      return await this.appService.getHistory(n);
    } catch (err) {
      throw new BadGatewayException(`Không đọc được lịch sử từ ThingSpeak: ${err.message}`);
    }
  }

  // ----------------------------------------------------------
  //  POST /api/pump/mode — đặt chế độ điều khiển bơm
  //  Body: { "mode": 0 | 1 | 2 }
  // ----------------------------------------------------------
  @Post('pump/mode')
  @HttpCode(HttpStatus.OK)
  async setPumpMode(@Body() body: SetModeDto) {
    const mode = body?.mode;

    if (mode === undefined || ![0, 1, 2].includes(Number(mode))) {
      throw new BadRequestException(
        'mode không hợp lệ. Chỉ chấp nhận: 0 (Auto), 1 (Bật), 2 (Tắt)',
      );
    }

    return this.appService.setPumpMode(Number(mode) as PumpMode);
  }
}
