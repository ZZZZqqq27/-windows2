前端 MVP 使用说明

1) 启动后端 HTTP API
./build/app --config configs/app.yaml --mode http

2) 启动静态前端
cd frontend
python3 -m http.server 8000

3) 浏览器打开
http://127.0.0.1:8000/

说明：
- 前端默认连接 http://127.0.0.1:8080，可在页面中修改。
- 上传时的“文件路径”是后端所在机器上的路径。
- 节点列表支持多节点管理，可添加多个 HTTP API 地址并切换使用。
