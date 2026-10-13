# ============================================================
#  模拟校园卡 Web 服务镜像
#  构建: docker build -t campus-card .
#  运行: docker run -p 8090:8090 campus-card
#  说明: 容器内服务监听 0.0.0.0，端口可用环境变量 PORT 覆盖
# ============================================================
FROM gcc:13 AS build
WORKDIR /app
COPY src ./src
COPY main.c ./
# 编译 Web 服务与命令行版，开启全部告警并视告警为零容忍（-Werror 可选）
RUN gcc -std=c11 -Wall -Wextra -O2 -Isrc src/server.c src/campus.c -o campus-server && \
    gcc -std=c11 -Wall -Wextra -O2 -Isrc main.c src/campus.c -o campus-cli

FROM debian:bookworm-slim
WORKDIR /app
# 只带运行所需文件：二进制、前端静态资源、数据目录
COPY --from=build /app/campus-server /app/campus-cli ./
COPY web ./web
RUN mkdir -p /app/data
ENV PORT=8090
EXPOSE 8090
# 数据目录挂载到卷，容器重建后卡片与记录不丢失
VOLUME ["/app/data"]
CMD ["/app/campus-server", "8090", "web", "data"]
