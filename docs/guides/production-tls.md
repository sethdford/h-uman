# Production TLS / Reverse Proxy Setup

SeaClaw gateway serves HTTP. Terminate TLS at a reverse proxy.

## Caddy (recommended)
```
seaclaw.example.com { reverse_proxy localhost:8420 }
```

## nginx + certbot
See nginx ssl config with proxy_pass to 127.0.0.1:8420.

## Cloudflare Tunnel
```bash
cloudflared tunnel --url http://localhost:8420
```
