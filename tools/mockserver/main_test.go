package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// testClient is the client half of the protocol, masked frames and all.
type testClient struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

func dial(t *testing.T, url string) *testClient {
	t.Helper()
	conn, err := net.Dial("tcp", strings.TrimPrefix(url, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	io.WriteString(conn, "GET /v1/overlay/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
	r := bufio.NewReader(conn)
	resp, err := http.ReadResponse(r, nil)
	if err != nil || resp.StatusCode != 101 {
		t.Fatalf("handshake: %v %v", resp, err)
	}
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("accept %q", got)
	}
	return &testClient{t: t, conn: conn, r: r}
}

func (c *testClient) send(v any) {
	b, _ := json.Marshal(v)
	hdr := []byte{0x81}
	if len(b) < 126 {
		hdr = append(hdr, 0x80|byte(len(b)))
	} else {
		hdr = append(hdr, 0x80|126, byte(len(b)>>8), byte(len(b)))
	}
	mask := [4]byte{1, 2, 3, 4}
	hdr = append(hdr, mask[:]...)
	for i := range b {
		b[i] ^= mask[i%4]
	}
	c.conn.Write(append(hdr, b...))
}

func (c *testClient) recv() map[string]any {
	c.t.Helper()
	c.conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	for {
		var h [2]byte
		if _, err := io.ReadFull(c.r, h[:]); err != nil {
			c.t.Fatal(err)
		}
		n := uint64(h[1] & 0x7F)
		if n == 126 {
			var e [2]byte
			io.ReadFull(c.r, e[:])
			n = uint64(binary.BigEndian.Uint16(e[:]))
		} else if n == 127 {
			var e [8]byte
			io.ReadFull(c.r, e[:])
			n = binary.BigEndian.Uint64(e[:])
		}
		p := make([]byte, n)
		io.ReadFull(c.r, p)
		if h[0]&0x0F != opText {
			continue
		}
		var m map[string]any
		if err := json.Unmarshal(p, &m); err != nil {
			c.t.Fatal(err)
		}
		return m
	}
}

func newTestServer(t *testing.T) (*server, string) {
	s := newServer("k", []string{"eu"})
	s.pageSize, s.maxConns, s.maxExtra = 2, 2, 1
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/overlay/ws", s.handleWS)
	ts := httptest.NewServer(mux)
	t.Cleanup(ts.Close)
	return s, ts.URL
}

func TestHistoryLiveAndResume(t *testing.T) {
	s, url := newTestServer(t)
	start := time.Now().UTC().Add(-time.Hour)
	s.add(ownID, "eu", 1, start.Add(-time.Minute), 0) // before the period
	for i := range 3 {
		s.add(ownID, "eu", 2, start.Add(time.Duration(i)*time.Minute), 0)
	}

	c := dial(t, url)
	c.send(map[string]any{"type": "hello", "protocol": 1, "key": "k"})
	if m := c.recv(); m["type"] != "welcome" {
		t.Fatal(m)
	}
	c.send(map[string]any{"type": "subscribe", "realm": "eu", "since": start.Unix()})
	var ids []float64
	for {
		m := c.recv()
		for _, b := range m["battles"].([]any) {
			ids = append(ids, b.(map[string]any)["id"].(float64))
		}
		if m["has_more"] == false {
			break
		}
	}
	if len(ids) != 3 {
		t.Fatalf("history %v", ids)
	}

	c.send(map[string]any{"type": "ping"})
	if m := c.recv(); m["type"] != "pong" {
		t.Fatal(m)
	}

	s.add(ownID, "eu", 3, time.Now().UTC(), 0)
	if m := c.recv(); m["type"] != "battles" {
		t.Fatal(m)
	}

	// Resume after the second record: the third and the live one come back.
	c2 := dial(t, url)
	c2.send(map[string]any{"type": "hello", "protocol": 1, "key": "k"})
	c2.recv()
	c2.send(map[string]any{"type": "subscribe", "realm": "eu", "since": start.Format(time.RFC3339), "after_id": ids[1]})
	m := c2.recv()
	if got := len(m["battles"].([]any)); got != 2 || m["has_more"] != false {
		t.Fatalf("resume %v", m)
	}
}

func TestConnectionLimitAndInitial(t *testing.T) {
	s, url := newTestServer(t)
	start := time.Now().UTC().Add(-time.Hour)
	glitch := s.add(ownID, "eu", 1, start, 454595)
	s.add(ownID, "eu", 1, start.Add(time.Minute), 0)
	s.records[0].initial = true

	c := dial(t, url)
	c.send(map[string]any{"type": "hello", "protocol": 1, "key": "k"})
	c.recv()
	c.send(map[string]any{"type": "subscribe", "realm": "eu", "since": start.Unix()})
	m := c.recv()
	if b := m["battles"].([]any); len(b) != 1 || b[0].(map[string]any)["id"].(float64) == float64(glitch.ID) {
		t.Fatalf("an initial record was sent: %v", m)
	}

	c2 := dial(t, url)
	c2.send(map[string]any{"type": "hello", "protocol": 1, "key": "k"})
	c2.recv()
	c3 := dial(t, url)
	c3.send(map[string]any{"type": "hello", "protocol": 1, "key": "k"})
	if m := c3.recv(); m["code"] != "rate_limited" || m["retry_after"] != float64(60) {
		t.Fatalf("third connection: %v", m)
	}
}

func TestWrongKey(t *testing.T) {
	_, url := newTestServer(t)
	c := dial(t, url)
	c.send(map[string]any{"type": "hello", "protocol": 1, "key": "nope"})
	if m := c.recv(); m["code"] != "key_invalid" || m["fatal"] != true {
		t.Fatal(m)
	}
}

func TestProtocol2OtherAccounts(t *testing.T) {
	s, url := newTestServer(t)
	c := dial(t, url)
	c.send(map[string]any{"type": "hello", "protocol": 2, "key": "k"})
	if m := c.recv(); m["type"] != "welcome" || m["max_extra_accounts"] != float64(1) {
		t.Fatal(m)
	}
	since := time.Now().UTC().Add(-time.Hour).Unix()

	// A new account: named, no reading yet, an empty history.
	c.send(map[string]any{"type": "subscribe", "realm": "com", "account_id": 512345678, "since": since})
	if m := c.recv(); m["type"] != "subscribed" || m["account_id"] != float64(512345678) || m["has_reading"] != false {
		t.Fatal(m)
	}
	if m := c.recv(); m["type"] != "history" || len(m["battles"].([]any)) != 0 || m["account_id"] != float64(512345678) {
		t.Fatal(m)
	}
	// Its battles reach it, named.
	s.add(512345678, "com", 2, time.Now().UTC(), 0)
	if m := c.recv(); m["type"] != "battles" || m["account_id"] != float64(512345678) {
		t.Fatal(m)
	}
	// Over the limit of one other account; unknown to Wargaming.
	c.send(map[string]any{"type": "subscribe", "realm": "eu", "account_id": 600000001, "since": since})
	if m := c.recv(); m["code"] != "too_many_accounts" || m["fatal"] != false || m["account_id"] != float64(600000001) {
		t.Fatal(m)
	}
	c.send(map[string]any{"type": "unsubscribe", "realm": "com", "account_id": 512345678})
	if m := c.recv(); m["type"] != "unsubscribed" {
		t.Fatal(m)
	}
	c.send(map[string]any{"type": "subscribe", "realm": "eu", "account_id": 600000999, "since": since})
	if m := c.recv(); m["code"] != "account_unknown" {
		t.Fatal(m)
	}
}

func TestProtocol1Server(t *testing.T) {
	s, url := newTestServer(t)
	s.protocol1 = true
	c := dial(t, url)
	c.send(map[string]any{"type": "hello", "protocol": 2, "key": "k"})
	if m := c.recv(); m["code"] != "protocol" || m["fatal"] != true {
		t.Fatal(m)
	}
	c1 := dial(t, url)
	c1.send(map[string]any{"type": "hello", "protocol": 1, "key": "k"})
	if m := c1.recv(); m["type"] != "welcome" {
		t.Fatal(m)
	}
	c1.send(map[string]any{"type": "subscribe", "realm": "eu", "since": time.Now().Add(-time.Hour).Unix()})
	if m := c1.recv(); m["type"] != "history" || m["account_id"] != nil {
		t.Fatalf("protocol 1 names no account: %v", m)
	}
}
