FROM node:20-slim

WORKDIR /app

# Sao chép package.json và cài đặt thư viện
COPY package*.json ./
RUN npm install --production

# Sao chép toàn bộ mã nguồn
COPY . .

# Mở cổng 3001 cho Dashboard
EXPOSE 3001

CMD ["npm", "start"]
