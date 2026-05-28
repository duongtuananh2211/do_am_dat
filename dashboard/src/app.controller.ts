import {
  Controller,
  Get,
  Post,
  Body,
  HttpCode,
  HttpStatus,
  BadRequestException,
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
