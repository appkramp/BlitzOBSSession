// Command keycheck says what the statistics server answers to a key, over
// protocols 1 and 2 — the first thing to look at when the plugin calls a key
// invalid. It prints the server's answers, never the key.
//
//	go run ./tools/keycheck ovk_…
//	go run ./tools/keycheck -server wss://stats.appkramp.com/v1/overlay/ws ovk_…
package main

import (
	"bufio"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
	"time"
)

var keyPattern = regexp.MustCompile(`^ovk_[A-Za-z0-9_-]{32,64}$`)

func main() {
	server := flag.String("server", "wss://stats.appkramp.com/v1/overlay/ws", "the overlay endpoint")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: keycheck [-server URL] KEY")
		os.Exit(2)
	}
	key := flag.Arg(0)
	fmt.Printf("key: %d characters, format %s\n", len(key), map[bool]string{true: "ok", false: "NOT ovk_ + 32–64 of [A-Za-z0-9_-]"}[keyPattern.MatchString(key)])
	for i, r := range key {
		if r > 0x7e || r < 0x21 {
			fmt.Printf("  character %d is U+%04X — not part of a key\n", i, r)
		}
	}
	for _, p := range []int{1, 2} {
		answer, err := hello(*server, key, p)
		if err != nil {
			fmt.Printf("protocol %d: %v\n", p, err)
			continue
		}
		fmt.Printf("protocol %d: %s\n", p, answer)
	}
}

// hello opens the WebSocket, sends hello and returns the first answer.
func hello(server, key string, protocol int) (string, error) {
	u, err := url.Parse(server)
	if err != nil {
		return "", err
	}
	host := u.Host
	if u.Port() == "" {
		host += map[string]string{"wss": ":443", "ws": ":80"}[u.Scheme]
	}
	var conn net.Conn
	if u.Scheme == "wss" {
		conn, err = tls.Dial("tcp", host, &tls.Config{ServerName: u.Hostname()})
	} else {
		conn, err = net.Dial("tcp", host)
	}
	if err != nil {
		return "", err
	}
	defer conn.Close()
	fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", u.RequestURI(), u.Hostname())
	r := bufio.NewReader(conn)
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		return "", err
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		return "", fmt.Errorf("HTTP %s", resp.Status)
	}

	b, _ := json.Marshal(map[string]any{"type": "hello", "protocol": protocol, "key": key, "client": "keycheck"})
	frame := []byte{0x81}
	if len(b) < 126 {
		frame = append(frame, 0x80|byte(len(b)))
	} else {
		frame = append(frame, 0x80|126, byte(len(b)>>8), byte(len(b)))
	}
	frame = append(frame, 0, 0, 0, 0) // a zero mask leaves the payload as is
	if _, err := conn.Write(append(frame, b...)); err != nil {
		return "", err
	}

	conn.SetReadDeadline(time.Now().Add(15 * time.Second))
	for {
		var h [2]byte
		if _, err := io.ReadFull(r, h[:]); err != nil {
			return "", err
		}
		n := uint64(h[1] & 0x7F)
		switch n {
		case 126:
			var e [2]byte
			io.ReadFull(r, e[:])
			n = uint64(binary.BigEndian.Uint16(e[:]))
		case 127:
			var e [8]byte
			io.ReadFull(r, e[:])
			n = binary.BigEndian.Uint64(e[:])
		}
		p := make([]byte, n)
		if _, err := io.ReadFull(r, p); err != nil {
			return "", err
		}
		if h[0]&0x0F == 1 {
			return strings.TrimSpace(string(p)), nil
		}
	}
}
