// Command obsshot saves what one OBS source renders, as a PNG, through the
// obs-websocket server built into OBS. It lets the overlay be checked in a
// real OBS without looking at the screen.
//
//	go run ./tools/obsshot Blitz overlay.png
//
// Needs Tools → WebSocket Server Settings: enabled, port 4455, no
// authentication.
package main

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

func main() {
	conn, err := net.Dial("tcp", "127.0.0.1:4455")
	if err != nil {
		fmt.Println("dial:", err)
		os.Exit(1)
	}
	io.WriteString(conn, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: obswebsocket.json\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
	r := bufio.NewReader(conn)
	resp, err := http.ReadResponse(r, nil)
	if err != nil || resp.StatusCode != 101 {
		fmt.Println("handshake", err)
		os.Exit(1)
	}
	send := func(v any) {
		b, _ := json.Marshal(v)
		hdr := []byte{0x81}
		switch {
		case len(b) < 126:
			hdr = append(hdr, 0x80|byte(len(b)))
		default:
			hdr = append(hdr, 0x80|126, byte(len(b)>>8), byte(len(b)))
		}
		hdr = append(hdr, 0, 0, 0, 0)
		conn.Write(append(hdr, b...))
	}
	recv := func() map[string]any {
		conn.SetReadDeadline(time.Now().Add(10 * time.Second))
		for {
			var h [2]byte
			if _, err := io.ReadFull(r, h[:]); err != nil {
				fmt.Println("read", err)
				os.Exit(1)
			}
			n := uint64(h[1] & 0x7F)
			if n == 126 {
				var e [2]byte
				io.ReadFull(r, e[:])
				n = uint64(binary.BigEndian.Uint16(e[:]))
			} else if n == 127 {
				var e [8]byte
				io.ReadFull(r, e[:])
				n = binary.BigEndian.Uint64(e[:])
			}
			p := make([]byte, n)
			io.ReadFull(r, p)
			if h[0]&0x0F != 1 {
				continue
			}
			var m map[string]any
			json.Unmarshal(p, &m)
			return m
		}
	}
	recv() // Hello
	send(map[string]any{"op": 1, "d": map[string]any{"rpcVersion": 1}})
	recv() // Identified
	send(map[string]any{"op": 6, "d": map[string]any{"requestType": "GetSourceScreenshot", "requestId": "1",
		"requestData": map[string]any{"sourceName": os.Args[1], "imageFormat": "png"}}})
	for {
		m := recv()
		if m["op"].(float64) != 7 {
			continue
		}
		d := m["d"].(map[string]any)
		st := d["requestStatus"].(map[string]any)
		if st["result"] != true {
			fmt.Println("failed:", st)
			os.Exit(1)
		}
		data := d["responseData"].(map[string]any)["imageData"].(string)
		raw, _ := base64.StdEncoding.DecodeString(data[strings.Index(data, ",")+1:])
		os.WriteFile(os.Args[2], raw, 0o644)
		fmt.Println("saved", len(raw), "bytes")
		return
	}
}
