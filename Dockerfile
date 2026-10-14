# ============================================================
#  模拟校园卡 Web 服务镜像（适配 Render / Railway / Fly.io 等容器平台）
#  本地构建: docker build -t campus-card .
#  本地运行: docker run -p 8090:8090 campus-card
#  平台部署: 平台会注入 PORT 环境变量，服务自动监听 0.0.0.0:$PORT
# ============================================================
FROM gcc:13 AS build
WORKDIR /app
COPY src ./src
COPY main.c ./
# 编译 Web 服务与命令行版；-Werror=implicit-function-declaration 防止漏引头文件
RUN gcc -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -O2 \
        -Isrc src/server.c src/campus.c -o campus-server && \
    gcc -std=c11 -Wall -Wextra -O2 -Isrc main.c src/campus.c -o campus-cli

FROM debian:bookworm-slim
WORKDIR /app
# 只带运行所需：两个二进制 + 前端静态资源
COPY --from=build /app/campus-server /app/campus-cli ./
COPY web ./web
# 数据目录由程序启动时自动创建（campus_init 内 ensure_dir），无需预先存在
ENV PORT=8090
EXPOSE 8090
# 注意：此处不声明 VOLUME。容器平台的文件系统本就是临时的，
# 声明匿名卷反而会让数据在重建时更容易丢失、也更难排查。
# 演示数据会在首次启动时自动生成（C001/C002/C003，密码 123456）。
CMD ["/app/campus-server", "8090", "web", "data"]
