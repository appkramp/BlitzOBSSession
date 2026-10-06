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
//
// Protocol 2 — other accounts on one key — as the real server serves it: any
// account id is accepted (one ending in 999 is "unknown to Wargaming"), a new
// one starts with no battles (has_reading false), at most -max-extra other
// accounts per connection. A battle for one of them:
//
//	curl -X POST 'http://127.0.0.1:8765/battle?account=512345678&realm=eu'
//
// -protocol1 makes it an old server that refuses protocol 2.
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

// The key's own account.
const ownID int64 = 1234567

var allRealms = []string{"eu", "com", "asia"}

type record struct {
	ID          int64          `json:"id"`
	TankID      int            `json:"tank_id"`
	Date        time.Time      `json:"date"`
	Count       int            `json:"count"`
	Approximate bool           `json:"approximate"`
	Values      map[string]any `json:"values"`
	account     int64
	realm       string
	initial     bool
}

type account struct {
	nickname   string
	clanTag    string
	hasReading bool
}

type server struct {
	key       string
	realms    []string // the own account's
	pageSize  int
	maxConns  int
	maxExtra  int
	protocol1 bool

	mu       sync.Mutex
	nextID   int64
	records  []record
	accounts map[int64]*account
	revoked  bool
	clients  map[*client]struct{}
}

type subKey struct {
	account int64
	realm   string
}

type subscription struct {
	since  time.Time
	lastID int64
}

type client struct {
	ws      *wsConn
	mu      sync.Mutex
	version int
	subs    map[subKey]*subscription
}

func newServer(key string, realms []string) *server {
	return &server{
		key:      key,
		realms:   realms,
		pageSize: 200,
		maxConns: 3,
		maxExtra: 10,
		accounts: map[int64]*account{ownID: {nickname: "MockStreamer", clanTag: "MOCK", hasReading: true}},
		clients:  map[*client]struct{}{},
	}
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8765", "listen address")
	key := flag.String("key", "ovk_dev_000000000000000000000000000000", "the one key accepted")
	every := flag.Duration("every", 45*time.Second, "how often an invented battle happens; 0 for never")
	hours := flag.Int("history", 30, "hours of invented history")
	page := flag.Int("page", 200, "records per history page")
	maxConns := flag.Int("max-conns", 3, "connections per key")
	maxExtra := flag.Int("max-extra", 10, "other accounts per connection")
	protocol1 := flag.Bool("protocol1", false, "act as a server that speaks protocol 1 only")
	realms := flag.String("realms", "eu,com", "realms the key's own account is on")
	flag.Parse()

	s := newServer(*key, strings.Split(*realms, ","))
	s.pageSize, s.maxConns, s.maxExtra, s.protocol1 = *page, *maxConns, *maxExtra, *protocol1

	now := time.Now().UTC()
	for _, realm := range s.realms {
		for t := now.Add(-time.Duration(*hours) * time.Hour); t.Before(now); t = t.Add(time.Duration(4+rand.IntN(10)) * time.Minute) {
			s.add(ownID, realm, tanks[rand.IntN(len(tanks))], t, 0)
		}
	}

	if *every > 0 {
		go func() {
			for range time.Tick(*every) {
				// Any account somebody watches, the own one included.
				s.mu.Lock()
				ids := []int64{}
				for id := range s.accounts {
					ids = append(ids, id)
				}
				s.mu.Unlock()
				id := ids[rand.IntN(len(ids))]
				r := s.add(id, s.realms[0], tanks[rand.IntN(len(tanks))], time.Now().UTC(), 0)
				log.Printf("battle %d: account %d tank %d", r.ID, id, r.TankID)
			}
		}()
	}

	http.HandleFunc("/v1/overlay/ws", s.handleWS)
	http.HandleFunc("POST /battle", func(w http.ResponseWriter, r *http.Request) {
		realm := r.URL.Query().Get("realm")
		if realm == "" {
			realm = s.realms[0]
		}
		id, _ := strconv.ParseInt(r.URL.Query().Get("account"), 10, 64)
		if id == 0 {
			id = ownID
		}
		tank, _ := strconv.Atoi(r.URL.Query().Get("tank_id"))
		if tank == 0 {
			tank = tanks[rand.IntN(len(tanks))]
		}
		damage, _ := strconv.Atoi(r.URL.Query().Get("damage"))
		rec := s.add(id, realm, tank, time.Now().UTC(), damage)
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

// add invents a battle of an account — with the given damage, when not 0 —
// and pushes it to the live subscribers of that account and realm.
func (s *server) add(id int64, realm string, tank int, at time.Time, damage int) record {
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
	if a := s.accounts[id]; a != nil {
		a.hasReading = true
	}
	s.nextID++
	rec := record{ID: s.nextID, TankID: tank, Date: at.Truncate(time.Second), Count: 1,
		Values: map[string]any{"all": all}, account: id, realm: realm}
	s.records = append(s.records, rec)
	for c := range s.clients {
		c.mu.Lock()
		if sub := c.subs[subKey{id, realm}]; sub != nil && !rec.Date.Before(sub.since) && rec.ID > sub.lastID {
			sub.lastID = rec.ID
			c.send(c.withAccount(map[string]any{"type": "battles", "realm": realm, "battles": []record{rec}}, id))
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

// withAccount names the account in protocol 2; protocol 1 names none.
func (c *client) withAccount(m map[string]any, id int64) map[string]any {
	if c.version >= 2 {
		m["account_id"] = id
	}
	return m
}

func (s *server) handleWS(w http.ResponseWriter, r *http.Request) {
	ws, err := upgrade(w, r)
	if err != nil {
		return
	}
	c := &client{ws: ws, subs: map[subKey]*subscription{}}
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
	if hello.Protocol != 1 && (hello.Protocol != 2 || s.protocol1) {
		fail("protocol", "unsupported protocol", true)
		return
	}
	c.version = hello.Protocol
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
	log.Printf("%s connected: %s, protocol %d", r.RemoteAddr, hello.Client, c.version)
	welcome := map[string]any{"type": "welcome", "protocol": c.version, "server_time": time.Now().UTC(),
		"account": map[string]any{"account_id": ownID, "nickname": "MockStreamer", "clan_tag": "MOCK", "realms": s.realms}}
	if c.version >= 2 {
		welcome["max_extra_accounts"] = s.maxExtra
		welcome["realms"] = allRealms
	}
	c.send(welcome)

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
			Type      string          `json:"type"`
			Realm     string          `json:"realm"`
			AccountID int64           `json:"account_id"`
			Since     json.RawMessage `json:"since"`
			AfterID   int64           `json:"after_id"`
		}
		if json.Unmarshal(raw, &msg) != nil {
			fail("bad_request", "not JSON", false)
			continue
		}
		id := msg.AccountID
		if id == 0 || c.version < 2 {
			id = ownID
		}
		subError := func(code, text string) {
			c.send(c.withAccount(map[string]any{"type": "error", "code": code, "message": text, "fatal": false,
				"realm": msg.Realm}, id))
		}
		switch msg.Type {
		case "ping":
			c.send(map[string]any{"type": "pong"})
		case "unsubscribe":
			c.mu.Lock()
			delete(c.subs, subKey{id, msg.Realm})
			c.mu.Unlock()
			c.send(map[string]any{"type": "unsubscribed", "account_id": id, "realm": msg.Realm})
		case "subscribe":
			since, err := parseTime(msg.Since)
			if err != nil {
				subError("bad_request", "since: "+err.Error())
				continue
			}
			realms := allRealms
			if id == ownID {
				realms = s.realms
			}
			if !slices.Contains(realms, msg.Realm) {
				subError("realm_unavailable", "realm "+msg.Realm+" is not available for this account")
				continue
			}
			if id%1000 == 999 {
				subError("account_unknown", "Wargaming does not know this account")
				continue
			}
			c.mu.Lock()
			others := map[int64]bool{}
			for k := range c.subs {
				if k.account != ownID {
					others[k.account] = true
				}
			}
			c.mu.Unlock()
			if id != ownID && !others[id] && len(others) >= s.maxExtra {
				subError("too_many_accounts", "the key shows as many other accounts as it may")
				continue
			}
			log.Printf("%s subscribe %d %s since %s after %d", r.RemoteAddr, id, msg.Realm, since.Format(time.RFC3339), msg.AfterID)
			s.subscribe(c, id, msg.Realm, since, msg.AfterID)
		default:
			fail("bad_request", "unknown message "+msg.Type, false)
		}
	}
}

// subscribe answers with the account, sends the history in pages and then
// marks the subscription live, under the server lock so no record written
// meanwhile is lost or sent twice.
func (s *server) subscribe(c *client, id int64, realm string, since time.Time, after int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c.mu.Lock()
	defer c.mu.Unlock()

	acc := s.accounts[id]
	if acc == nil {
		// A new account: it joins the pool, its first reading is only a
		// baseline, so there are no battles yet.
		acc = &account{nickname: fmt.Sprintf("Player%d", id%100000)}
		s.accounts[id] = acc
	}
	if c.version >= 2 {
		c.send(map[string]any{"type": "subscribed", "account_id": id, "realm": realm, "nickname": acc.nickname,
			"clan_tag": acc.clanTag, "collecting": true, "has_reading": acc.hasReading})
	}

	var hist []record
	for _, r := range s.records {
		if r.account == id && r.realm == realm && !r.initial && !r.Date.Before(since) && r.ID > after {
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
		c.send(c.withAccount(map[string]any{"type": "history", "realm": realm, "battles": page, "has_more": end < len(hist)}, id))
		if end >= len(hist) {
			break
		}
	}
	sub := &subscription{since: since, lastID: after}
	for _, r := range hist {
		sub.lastID = max(sub.lastID, r.ID)
	}
	c.subs[subKey{id, realm}] = sub
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
