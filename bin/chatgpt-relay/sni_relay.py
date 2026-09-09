#!/usr/bin/env python3
# SNI 透明中继：把指向 127.0.0.1:443 的 TLS 流量按 SNI 域名 CONNECT 转发给 Clash 代理
# 用于绕过 WSL 内进程（codex app-server）不走代理 + 本地 DNS 污染的双重问题
import socket, struct, select, threading, logging

PROXY_HOST, PROXY_PORT = '127.0.0.1', 7897
logging.basicConfig(filename='/tmp/sni_relay.log', level=logging.INFO,
                    format='%(asctime)s %(levelname)s %(message)s')


def parse_sni(data):
    """从 TLS ClientHello 缓冲中解析 SNI 域名；数据不足返回 None。"""
    if len(data) < 5 or data[0] != 0x16:
        return None
    rec_len = struct.unpack('>H', data[3:5])[0]
    if len(data) < 5 + rec_len:
        return None
    hs = data[5:]
    if len(hs) < 4 or hs[0] != 0x01:
        return None
    p = 4
    if p + 2 + 32 + 1 > len(hs):
        return None
    p += 2 + 32
    sl = hs[p]; p += 1
    p += sl
    if p + 2 > len(hs):
        return None
    cl = struct.unpack('>H', hs[p:p + 2])[0]; p += 2 + cl
    if p + 1 > len(hs):
        return None
    cm = hs[p]; p += 1 + cm
    if p + 2 > len(hs):
        return None
    ext_len = struct.unpack('>H', hs[p:p + 2])[0]
    p += 2
    end = p + ext_len
    while p + 4 <= min(end, len(hs)):
        et = struct.unpack('>H', hs[p:p + 2])[0]
        el = struct.unpack('>H', hs[p + 2:p + 4])[0]
        p += 4
        if et == 0:  # SNI: data = list_len(2) + type(1) + name_len(2) + name
            if p + 5 <= min(end, len(hs)):
                nl = struct.unpack('>H', hs[p + 3:p + 5])[0]
                if p + 5 + nl <= min(end, len(hs)):
                    return hs[p + 5:p + 5 + nl].decode('ascii', 'ignore')
        p += el
    return None


def pump(a, b):
    try:
        while True:
            r, _, _ = select.select([a, b], [], [], 60)
            if not r:
                return
            for s in r:
                d = s.recv(65536)
                if not d:
                    return
                (b if s is a else a).sendall(d)
    except Exception:
        pass
    finally:
        for s in (a, b):
            try:
                s.close()
            except Exception:
                pass


def handle(conn):
    try:
        conn.settimeout(15)
        buf = b''
        sni = None
        while len(buf) < 65536:
            d = conn.recv(4096)
            if not d:
                break
            buf += d
            sni = parse_sni(buf)
            if sni:
                break
        if not sni:
            logging.info('drop: no SNI')
            conn.close()
            return
        up = socket.create_connection((PROXY_HOST, PROXY_PORT), timeout=10)
        req = 'CONNECT %s:443 HTTP/1.1\r\nHost: %s:443\r\n\r\n' % (sni, sni)
        up.sendall(req.encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            d = up.recv(4096)
            if not d:
                raise IOError('proxy closed')
            resp += d
        head = resp.split(b'\r\n', 1)[0]
        if b' 200 ' not in head:
            logging.info('proxy reject %s: %r', sni, head)
            up.close()
            conn.close()
            return
        logging.info('relay %s ok', sni)
        up.sendall(buf)  # 回放已缓冲的 ClientHello
        conn.settimeout(None)
        up.settimeout(None)
        pump(conn, up)
    except Exception as e:
        logging.info('err: %r', e)
        try:
            conn.close()
        except Exception:
            pass


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', 443))
    srv.listen(128)
    logging.info('SNI relay listening on 443')
    while True:
        c, _ = srv.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == '__main__':
    main()
