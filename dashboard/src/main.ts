import 'dotenv/config';
import { NestFactory } from '@nestjs/core';
import { NestExpressApplication } from '@nestjs/platform-express';
import { AppModule } from './app.module';
import { join } from 'path';

async function bootstrap() {
  const app = await NestFactory.create<NestExpressApplication>(AppModule);

  // Phục vụ file tĩnh từ thư mục public/
  app.useStaticAssets(join(__dirname, '..', 'public'));

  // Cho phép CORS nếu cần truy cập từ domain khác
  app.enableCors();

  const port = process.env.PORT || 3000;
  await app.listen(port);

  console.log(`\n🌱 Dashboard đang chạy tại: http://localhost:${port}`);
  console.log(`📡 API: http://localhost:${port}/api/status\n`);
}
bootstrap();
