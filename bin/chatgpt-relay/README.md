# ChatGPT WSL 后端网络修复（SNI 中继）

## 问题根因（2026-09-09 诊断）

1. Windows 桌面 ChatGPT/Codex 应用在 WSL 里跑 `codex app-server`（CODEX_HOME=/mnt/c/Users/34073/.codex）
2. 该 app-server 的出站请求**不走** `http_proxy` 环境变量（其网络 client 未启用 env 代理），全部直连 443
3. 本地 DNS 链路对 OpenAI 域名做**轮换式污染**（chatgpt.com→199.96.63.75、ab.chatgpt.com→162.125.83.1 Dropbox、api.openai.com→198.44.x 等），直连虚假 IP 被墙
4. 表现：UI 报 "Connection failed: error sending request"；日志 `error sending request for url (https://chatgpt.com/backend-api/...)`

## 修复原理

- `/etc/hosts` 把 OpenAI 相关域全部指向 `127.0.0.1`
- `sni_relay.py` 监听 443：解析 TLS ClientHello 的 SNI → 向 Clash 混合端口（127.0.0.1:7897）发 CONNECT → 双向转发
- 域名解析由 Clash 完成（真实 IP），流量走 AI 策略组节点（当前 🇯🇵 Japan 01）

## hosts 条目（/etc/hosts）

```
127.0.0.1 chatgpt.com ab.chatgpt.com auth.openai.com api.openai.com files.oaiusercontent.com chatgpt.com.cdn.cloudflare.net
```

## 使用

启动：`setsid nohup python3 /usr/local/lib/chatgpt-relay/sni_relay.py >/dev/null 2>&1 &`
日志：`/tmp/sni_relay.log`
systemd 服务文件已备：`/etc/systemd/system/chatgpt-relay.service`（验证稳定后 `systemctl enable --now`）

## 备注

- 该问题不影响走代理的请求（curl -x 127.0.0.1:7897 一直正常）
- Clash 控制 API 经 named pipe `\\.\pipe\verge-mihomo`（9097 无 TCP 监听），辅助脚本见当时会话
