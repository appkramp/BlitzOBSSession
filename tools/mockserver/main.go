// Command mockserver speaks docs/overlay-protocol.md with invented battles,
// so the plugin can be developed without the real server.
//
//	go run ./tools/mockserver -key ovk_dev_000000000000000000000000000000
//
// Then in the plugin's settings: server ws://127.0.0.1:8765, that key.
//
// It fills the last few hours with history and adds a battle every -every.
// A battle can also be added by hand:
//
//	curl -X POST 'http://127.0.0.1:8765/battle?realm=eu&tank_id=3649'
//
// and the connected clients can be dropped, to test resuming:
//
//	curl -X POST http://127.0.0.1:8765/drop
//	curl -X POST http://127.0.0.1:8765/revoke     # the key stops working
//
// A glitch, as the real server gets them from Wargaming, and its correction:
//
//	curl -X POST 'http://127.0.0.1:8765/battle?damage=454595'   # prints its id
//	curl -X POST 'http://127.0.0.1:8765/initial?id=1234'
//
// A record marked initial is no longer sent, but — as on the real server —
// not taken back from a client that already has it: the plugin's "Load the
// statistics again" does that.
//
// It behaves as the real server does where that differs from the protocol
// document (StatsCollector docs/overlay.md): history in pages of 200 ordered
// by id, at most -max-conns connections per key.
package main

import (
	"cmp"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math/rand/v2"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Real tank ids from the vehicle catalogue: every class, tiers 6, 8 and 10.
var tanks = []int{1553, 4961, 3937, 801, 817, 4481, 2609, 3889, 3649, 593, 49, 3681}

type record struct {
	ID          int64          `json:"id"`
	TankID      int            `json:"tank_id"`
	Date        time.Time      `json:"date"`
	Count       int            `json:"count"`
	Approximate bool           `json:"approximate"`
	Values      map[string]any `json:"values"`
	realm       string
	initial     bool
}

type server struct {
	key      string
	account  map[string]any
	realms   []string
	pageSize int
	maxConns int

	mu      sync.Mutex
	nextID  int64
	records []record
	revoked bool
	clients map[*client]struct{}
}

type client struct {
	ws     *wsConn
	mu     sync.Mutex
	realm  string
	since  time.Time
	lastID int64
	live   bool
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8765", "listen address")
	key := flag.String("key", "ovk_dev_000000000000000000000000000000", "the one key accepted")
	every := flag.Duration("every", 45*time.Second, "how often an invented battle happens; 0 for never")
	hours := flag.Int("history", 30, "hours of invented history")
	page := flag.Int("page", 200, "records per history page")
	maxConns := flag.Int("max-conns", 3, "connections per key")
	realms := flag.String("realms", "eu,com", "realms the key opens")
	flag.Parse()

	s := &server{
		key:      *key,
		realms:   strings.Split(*realms, ","),
		pageSize: *page,
		maxConns: *maxConns,
		nextID:   1000,
		clients:  map[*client]struct{}{},
	}
	s.account = map[string]any{"account_id": 1234567, "nickname": "MockStreamer", "clan_tag": "MOCK", "realms": s.realms}

	now := time.Now().UTC()
	for _, realm := range s.realms {
		for t := now.Add(-time.Duration(*hours) * time.Hour); t.Before(now); t = t.Add(time.Duration(4+rand.IntN(10)) * time.Minute) {
			s.add(realm, tanks[rand.IntN(len(tanks))], t, 0)
		}
	}

	if *every > 0 {
		go func() {
			for range time.Tick(*every) {
				r := s.add(s.realms[0], tanks[rand.IntN(len(tanks))], time.Now().UTC(), 0)
				log.Printf("battle %d: tank %d", r.ID, r.TankID)
			}
		}()
	}

	http.HandleFunc("/v1/overlay/ws", s.handleWS)
	http.HandleFunc("POST /battle", func(w http.ResponseWriter, r *http.Request) {
		realm := r.URL.Query().Get("realm")
		if realm == "" {
			realm = s.realms[0]
		}
		tank, _ := strconv.Atoi(r.URL.Query().Get("tank_id"))
		if tank == 0 {
			tank = tanks[rand.IntN(len(tanks))]
		}
		damage, _ := strconv.Atoi(r.URL.Query().Get("damage"))
		rec := s.add(realm, tank, time.Now().UTC(), damage)
		fmt.Fprintf(w, "added %d\n", rec.ID)
	})
	http.HandleFunc("POST /initial", func(w http.ResponseWriter, r *http.Request) {
		id, _ := strconv.ParseInt(r.URL.Query().Get("id"), 10, 64)
		s.mu.Lock()
		defer s.mu.Unlock()
		for i := range s.records {
			if s.records[i].ID == id {
				s.records[i].initial = true
				fmt.Fprintf(w, "record %d marked initial\n", id)
				return
			}
		}
		http.Error(w, "no such record", http.StatusNotFound)
	})
	http.HandleFunc("POST /drop", func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		for c := range s.clients {
			c.ws.conn.Close()
		}
		s.mu.Unlock()
		fmt.Fprintln(w, "dropped")
	})
	http.HandleFunc("POST /revoke", func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.revoked = true
		for c := range s.clients {
			c.send(map[string]any{"type": "error", "code": "key_invalid", "message": "the key was replaced", "fatal": true})
			c.ws.Close(4001, "key_invalid")
		}
		s.mu.Unlock()
		fmt.Fprintln(w, "revoked")
	})
	log.Printf("listening on ws://%s/v1/overlay/ws, key %s", *addr, *key)
	log.Fatal(http.ListenAndServe(*addr, nil))
}

// add invents a battle — with the given damage, when not 0 — and pushes it to
// the live subscribers of its realm.
func (s *server) add(realm string, tank int, at time.Time, damage int) record {
	win := rand.IntN(100) < 52
	survived := rand.IntN(100) < 40
	shots := 4 + rand.IntN(10)
	hits := shots * (55 + rand.IntN(40)) / 100
	all := map[string]any{
		"battles": 1, "wins": b2i(win), "losses": b2i(!win),
		"damage_dealt": cmp.Or(damage, 400+rand.IntN(3000)), "damage_received": rand.IntN(2000),
		"hits": hits, "shots": shots, "spotted": rand.IntN(3),
		"frags": rand.IntN(4), "frags8p": rand.IntN(2), "xp": 300 + rand.IntN(1200),
		"survived_battles": b2i(survived), "win_and_survived": b2i(win && survived),
		"capture_points": 0, "dropped_capture_points": 0,
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.nextID++
	rec := record{ID: s.nextID, TankID: tank, Date: at.Truncate(time.Second), Count: 1,
		Values: map[string]any{"all": all}, realm: realm}
	s.records = append(s.records, rec)
	for c := range s.clients {
		c.mu.Lock()
		if c.live && c.realm == realm && !rec.Date.Before(c.since) && rec.ID > c.lastID {
			c.lastID = rec.ID
			c.send(map[string]any{"type": "battles", "realm": realm, "battles": []record{rec}})
		}
		c.mu.Unlock()
	}
	return rec
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func (c *client) send(v any) {
	b, _ := json.Marshal(v)
	if err := c.ws.WriteText(b); err != nil {
		c.ws.conn.Close()
	}
}

func (s *server) handleWS(w http.ResponseWriter, r *http.Request) {
	ws, err := upgrade(w, r)
	if err != nil {
		return
	}
	c := &client{ws: ws}
	defer ws.conn.Close()

	fail := func(code, msg string, fatal bool) {
		c.send(map[string]any{"type": "error", "code": code, "message": msg, "fatal": fatal})
		if fatal {
			ws.Close(4000, code)
		}
	}

	ws.conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	raw, err := ws.ReadMessage()
	if err != nil {
		return
	}
	var hello struct {
		Type     string `json:"type"`
		Protocol int    `json:"protocol"`
		Key      string `json:"key"`
		Client   string `json:"client"`
	}
	if json.Unmarshal(raw, &hello) != nil || hello.Type != "hello" {
		fail("bad_request", "the first message must be hello", true)
		return
	}
	if hello.Protocol != 1 {
		fail("protocol", "protocol 1 only", true)
		return
	}
	s.mu.Lock()
	ok := hello.Key == s.key && !s.revoked
	s.mu.Unlock()
	if !ok {
		fail("key_invalid", "unknown key", true)
		return
	}
	s.mu.Lock()
	full := len(s.clients) >= s.maxConns
	s.mu.Unlock()
	if full {
		c.send(map[string]any{"type": "error", "code": "rate_limited", "message": "too many connections with this key",
			"fatal": true, "retry_after": 60})
		ws.Close(4000, "rate_limited")
		return
	}
	log.Printf("%s connected: %s", r.RemoteAddr, hello.Client)
	c.send(map[string]any{"type": "welcome", "protocol": 1, "server_time": time.Now().UTC(), "account": s.account})

	s.mu.Lock()
	s.clients[c] = struct{}{}
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.clients, c)
		s.mu.Unlock()
		log.Printf("%s disconnected", r.RemoteAddr)
	}()

	go func() {
		for range time.Tick(25 * time.Second) {
			if ws.Ping() != nil {
				return
			}
		}
	}()

	for {
		ws.conn.SetReadDeadline(time.Now().Add(60 * time.Second))
		raw, err := ws.ReadMessage()
		if err != nil {
			return
		}
		var msg struct {
			Type    string          `json:"type"`
			Realm   string          `json:"realm"`
			Since   json.RawMessage `json:"since"`
			AfterID int64           `json:"after_id"`
		}
		if json.Unmarshal(raw, &msg) == nil && msg.Type == "ping" {
			c.send(map[string]any{"type": "pong"})
			continue
		}
		if json.Unmarshal(raw, &msg) != nil || msg.Type != "subscribe" {
			fail("bad_request", "expected subscribe", false)
			continue
		}
		since, err := parseTime(msg.Since)
		if err != nil {
			fail("bad_request", "since: "+err.Error(), false)
			continue
		}
		if !slices.Contains(s.realms, msg.Realm) {
			fail("realm_unavailable", "realm "+msg.Realm+" is not collected for this account", false)
			continue
		}
		log.Printf("%s subscribe %s since %s after %d", r.RemoteAddr, msg.Realm, since.Format(time.RFC3339), msg.AfterID)
		s.subscribe(c, msg.Realm, since, msg.AfterID)
	}
}

// subscribe sends the history in pages and then marks the client live, under
// the server lock so no record written meanwhile is lost or sent twice.
func (s *server) subscribe(c *client, realm string, since time.Time, after int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c.mu.Lock()
	defer c.mu.Unlock()
	var hist []record
	for _, r := range s.records {
		if r.realm == realm && !r.initial && !r.Date.Before(since) && r.ID > after {
			hist = append(hist, r)
		}
	}
	// By id, as the real server sends them: id is the resume cursor.
	slices.SortFunc(hist, func(a, b record) int { return cmp.Compare(a.ID, b.ID) })
	for i := 0; ; i += s.pageSize {
		end := min(i+s.pageSize, len(hist))
		page := hist[i:end]
		if page == nil {
			page = []record{}
		}
		c.send(map[string]any{"type": "history", "realm": realm, "battles": page, "has_more": end < len(hist)})
		if end >= len(hist) {
			break
		}
	}
	c.realm, c.since, c.live = realm, since, true
	c.lastID = after
	for _, r := range hist {
		c.lastID = max(c.lastID, r.ID)
	}
}

func parseTime(raw json.RawMessage) (time.Time, error) {
	var s string
	if json.Unmarshal(raw, &s) == nil {
		return time.Parse(time.RFC3339, s)
	}
	var n int64
	if err := json.Unmarshal(raw, &n); err != nil {
		return time.Time{}, fmt.Errorf("neither RFC 3339 nor unix seconds")
	}
	return time.Unix(n, 0).UTC(), nil
}
