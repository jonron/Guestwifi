// Enhetstester för GuestCore. Kör med: pio test -e native
#include <ArduinoJson.h>
#include <GuestCore.h>
#include <unity.h>

#include <cctype>
#include <deque>
#include <random>
#include <set>

using namespace guest;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- ordlista
static void test_wordlist_is_clean() {
  TEST_ASSERT_TRUE(kWordCount >= 300);
  std::set<std::string> seen;
  for (size_t i = 0; i < kWordCount; ++i) {
    std::string w = kWords[i];
    TEST_ASSERT_TRUE_MESSAGE(w.size() >= 3 && w.size() <= 7, w.c_str());
    for (char c : w) TEST_ASSERT_TRUE_MESSAGE(c >= 'a' && c <= 'z', w.c_str());
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(w).second, w.c_str());
  }
}

// ---------------------------------------------------------------- lösenord
static bool isWord(const std::string& capitalized) {
  std::string w = capitalized;
  if (w.empty() || !std::isupper(static_cast<unsigned char>(w[0]))) return false;
  w[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(w[0])));
  for (size_t i = 0; i < kWordCount; ++i)
    if (w == kWords[i]) return true;
  return false;
}

static void test_password_format() {
  std::mt19937 mt(42);
  PasswordGenerator gen([&] { return static_cast<uint32_t>(mt()); });
  for (int i = 0; i < 2000; ++i) {
    std::string pw = gen.generate();
    size_t d1 = pw.find('-');
    size_t d2 = pw.find('-', d1 + 1);
    TEST_ASSERT_TRUE(d1 != std::string::npos && d2 != std::string::npos);
    std::string a = pw.substr(0, d1), b = pw.substr(d1 + 1, d2 - d1 - 1), n = pw.substr(d2 + 1);
    TEST_ASSERT_TRUE_MESSAGE(isWord(a), pw.c_str());
    TEST_ASSERT_TRUE_MESSAGE(isWord(b), pw.c_str());
    TEST_ASSERT_TRUE_MESSAGE(a != b, pw.c_str());
    TEST_ASSERT_EQUAL(2, n.size());
    for (char c : n) TEST_ASSERT_TRUE_MESSAGE(c >= '2' && c <= '9', pw.c_str());
    TEST_ASSERT_TRUE(pw.size() >= 8 && pw.size() <= 63);  // WPA-gränser
  }
}

static void test_password_never_repeats_previous() {
  // RNG som alltid ger samma följd -> samma lösenord två gånger i rad.
  uint32_t seq[] = {0, 1, 0, 0, 0, 1, 0, 0, 5, 7, 3, 3};
  size_t i = 0;
  PasswordGenerator gen([&] { return seq[i++ % 12]; });
  std::string first = gen.generate();
  i = 0;
  std::string second = gen.generate(first);
  TEST_ASSERT_TRUE(first != second);
}

static void test_uniform_rejects_biased_values() {
  std::deque<uint32_t> values = {UINT32_MAX, UINT32_MAX - 3, 10};
  PasswordGenerator gen([&] {
    uint32_t v = values.front();
    values.pop_front();
    return v;
  });
  // n=7: 2^32 mod 7 = 4, så de fyra högsta värdena ligger i den ofullständiga hinken.
  TEST_ASSERT_EQUAL_UINT32(10 % 7, gen.uniform(7));
  TEST_ASSERT_TRUE(values.empty());
}

static void test_password_distribution_roughly_uniform() {
  std::mt19937 mt(7);
  PasswordGenerator gen([&] { return static_cast<uint32_t>(mt()); });
  int counts[8] = {0};
  for (int i = 0; i < 80000; ++i) counts[gen.uniform(8)]++;
  for (int c : counts) TEST_ASSERT_INT_WITHIN(600, 10000, c);
}

// ---------------------------------------------------------------- HTTP
static void test_parse_content_length_and_cookies() {
  std::string raw =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: TOKEN=abc; path=/\r\n"
      "Set-Cookie: other=1\r\nX-CSRF-Token: c1\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
  HttpResponse r;
  std::string err;
  TEST_ASSERT_TRUE(parseResponse(raw, r, err));
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_EQUAL_STRING("hello", r.body.c_str());
  TEST_ASSERT_EQUAL(2, r.setCookies.size());
  TEST_ASSERT_EQUAL_STRING("c1", r.header("x-csrf-token").c_str());
}

static void test_parse_chunked() {
  std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "4\r\n{\"a\"\r\nA;ext=1\r\n:12345678}\r\n0\r\n\r\n";
  HttpResponse r;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseResponse(raw, r, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"a\":12345678}", r.body.c_str());
}

static void test_parse_rejects_garbage() {
  HttpResponse r;
  std::string err;
  TEST_ASSERT_FALSE(parseResponse("", r, err));
  TEST_ASSERT_FALSE(parseResponse("FOO 200\r\n\r\n", r, err));
  TEST_ASSERT_FALSE(parseResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", r, err));
  TEST_ASSERT_FALSE(
      parseResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nff\r\nabc", r, err));
}

static void test_serialize_request() {
  HttpRequest req{"PUT", "/x", {{"Cookie", "TOKEN=t"}}, "{\"a\":1}"};
  std::string s = serializeRequest(req, "192.168.40.1");
  TEST_ASSERT_TRUE(s.rfind("PUT /x HTTP/1.1\r\nHost: 192.168.40.1\r\n", 0) == 0);
  TEST_ASSERT_TRUE(s.find("Content-Length: 7\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(s.find("Cookie: TOKEN=t\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(s.find("\r\n\r\n{\"a\":1}") != std::string::npos);
}

// ---------------------------------------------------------------- UniFi-klient
// Enkel UDM-attrapp i minnet som beter sig som UniFi OS.
struct FakeUdm : HttpTransport {
  std::string guestPass = "Old-Pass-22";
  std::string token = "tok1";
  std::string csrf = "csrf1";
  bool expireSessionOnce = false;
  bool rotateCsrf = false;
  bool rejectPut = false;
  int logins = 0, puts = 0;

  static HttpResponse json(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.body = body;
    return r;
  }

  bool send(const HttpRequest& req, HttpResponse& resp, std::string&) override {
    auto hdr = [&](const std::string& n) {
      for (auto& h : req.headers)
        if (h.first == n) return h.second;
      return std::string();
    };
    if (req.method == "POST" && req.path == "/api/auth/login") {
      logins++;
      JsonDocument d;
      deserializeJson(d, req.body);
      if (std::string(d["username"] | "") != "admin" || std::string(d["password"] | "") != "pw") {
        resp = json(401, "{}");
        return true;
      }
      resp = json(200, "{}");
      resp.setCookies.push_back("TOKEN=" + token + "; path=/; secure; httponly");
      resp.headers["x-csrf-token"] = csrf;
      return true;
    }
    if (hdr("Cookie") != "TOKEN=" + token || expireSessionOnce) {
      expireSessionOnce = false;
      resp = json(401, R"({"meta":{"rc":"error","msg":"api.err.LoginRequired"},"data":[]})");
      return true;
    }
    const std::string base = "/proxy/network/api/s/default/rest/wlanconf";
    if (req.method == "GET" && req.path == base) {
      resp = json(200, R"({"meta":{"rc":"ok"},"data":[)"
                       R"({"_id":"iot1","name":"VidebergKraft_IOT","x_passphrase":"secret","enabled":true},)"
                       R"({"_id":"g1","name":"VidebergKraft_Guest","x_passphrase":")" +
                           guestPass + R"(","security":"wpapsk"}]})");
      return true;
    }
    if (req.method == "PUT" && req.path == base + "/g1") {
      if (hdr("X-CSRF-Token") != csrf) {
        resp = json(403, "{}");
        return true;
      }
      puts++;
      if (rejectPut) {
        resp = json(200, R"({"meta":{"rc":"error","msg":"api.err.Invalid"},"data":[]})");
        return true;
      }
      JsonDocument d;
      deserializeJson(d, req.body);
      guestPass = d["x_passphrase"] | "";
      resp = json(200, R"({"meta":{"rc":"ok"},"data":[]})");
      if (rotateCsrf) {
        csrf = "csrf2";
        resp.headers["x-updated-csrf-token"] = csrf;
      }
      return true;
    }
    resp = json(404, "{}");
    return true;
  }
};

static void test_unifi_read_and_write() {
  FakeUdm udm;
  UnifiClient c(udm, "admin", "pw");
  std::string pw;
  TEST_ASSERT_TRUE_MESSAGE(c.readPassphrase("VidebergKraft_Guest", pw), c.lastError().c_str());
  TEST_ASSERT_EQUAL_STRING("Old-Pass-22", pw.c_str());
  TEST_ASSERT_TRUE_MESSAGE(c.writePassphrase("VidebergKraft_Guest", "New-Pass-33"),
                           c.lastError().c_str());
  TEST_ASSERT_EQUAL_STRING("New-Pass-33", udm.guestPass.c_str());
  TEST_ASSERT_EQUAL(1, udm.logins);
}

static void test_unifi_relogin_on_expired_session() {
  FakeUdm udm;
  UnifiClient c(udm, "admin", "pw");
  std::string pw;
  TEST_ASSERT_TRUE(c.readPassphrase("VidebergKraft_Guest", pw));
  udm.expireSessionOnce = true;
  TEST_ASSERT_TRUE_MESSAGE(c.readPassphrase("VidebergKraft_Guest", pw), c.lastError().c_str());
  TEST_ASSERT_EQUAL(2, udm.logins);
}

static void test_unifi_follows_rotated_csrf() {
  FakeUdm udm;
  udm.rotateCsrf = true;
  UnifiClient c(udm, "admin", "pw");
  TEST_ASSERT_TRUE(c.writePassphrase("VidebergKraft_Guest", "Aaa-Bbb-22"));
  TEST_ASSERT_TRUE_MESSAGE(c.writePassphrase("VidebergKraft_Guest", "Ccc-Ddd-33"),
                           c.lastError().c_str());
  TEST_ASSERT_EQUAL(1, udm.logins);  // ingen ny inloggning behövdes
  TEST_ASSERT_EQUAL(2, udm.puts);
}

static void test_unifi_errors() {
  FakeUdm udm;
  UnifiClient bad(udm, "admin", "wrong");
  std::string pw;
  TEST_ASSERT_FALSE(bad.readPassphrase("VidebergKraft_Guest", pw));
  TEST_ASSERT_TRUE(bad.lastError().find("HTTP 401") != std::string::npos);

  UnifiClient c(udm, "admin", "pw");
  TEST_ASSERT_FALSE(c.readPassphrase("NoSuchSsid", pw));
  TEST_ASSERT_TRUE(c.lastError().find("NoSuchSsid") != std::string::npos);

  udm.rejectPut = true;
  TEST_ASSERT_FALSE(c.writePassphrase("VidebergKraft_Guest", "Xxx-Yyy-44"));
  TEST_ASSERT_TRUE(c.lastError().find("api.err.Invalid") != std::string::npos);

  UnifiClient none(udm, "", "");
  TEST_ASSERT_FALSE(none.readPassphrase("VidebergKraft_Guest", pw));
}

// ---------------------------------------------------------------- rotation
struct FakeWlan : WlanApi {
  std::string current = "Old-Pass-22";
  bool writeReturns = true;     // vad writePassphrase rapporterar
  bool writeApplies = true;     // om skrivningen faktiskt slår igenom
  int applyAfterReads = 0;      // routern visar nya värdet först efter N läsningar
  int readFailures = 0;         // så många läsningar misslyckas först
  std::string pending;
  int reads = 0;
  std::string err = "simulerat fel";

  bool readPassphrase(const std::string&, std::string& p) override {
    reads++;
    if (readFailures > 0) {
      readFailures--;
      return false;
    }
    if (!pending.empty() && applyAfterReads-- <= 0) {
      current = pending;
      pending.clear();
    }
    p = current;
    return true;
  }
  bool writePassphrase(const std::string&, const std::string& p) override {
    if (writeApplies) pending = p;
    return writeReturns;
  }
  const std::string& lastError() const override { return err; }
};

struct RotFixture {
  std::mt19937 mt{1};
  PasswordGenerator gen{[this] { return static_cast<uint32_t>(mt()); }};
  FakeWlan wlan;
  int sleeps = 0;
  Rotator rot{wlan, gen, "VidebergKraft_Guest", [this](uint32_t) { sleeps++; }, 5, 1000};
};

static void test_rotate_success() {
  RotFixture f;
  RotateResult r = f.rot.rotate("Old-Pass-22");
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error.c_str());
  TEST_ASSERT_EQUAL_STRING(f.wlan.current.c_str(), r.password.c_str());
  TEST_ASSERT_TRUE(r.password != "Old-Pass-22");
}

static void test_rotate_waits_for_router_to_apply() {
  RotFixture f;
  f.wlan.applyAfterReads = 3;
  f.wlan.readFailures = 1;  // t.ex. WiFi-glapp när AP:n omprovisioneras
  RotateResult r = f.rot.rotate("Old-Pass-22");
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error.c_str());
  TEST_ASSERT_EQUAL(5, f.wlan.reads);
}

static void test_rotate_lost_response_but_applied() {
  RotFixture f;
  f.wlan.writeReturns = false;  // svaret tappades, men routern bytte ändå
  RotateResult r = f.rot.rotate("Old-Pass-22");
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error.c_str());
}

static void test_rotate_write_failed() {
  RotFixture f;
  f.wlan.writeReturns = false;
  f.wlan.writeApplies = false;
  RotateResult r = f.rot.rotate("Old-Pass-22");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(1, f.wlan.reads);  // ingen onödig väntan
  TEST_ASSERT_EQUAL_STRING("Old-Pass-22", f.wlan.current.c_str());
  TEST_ASSERT_TRUE(r.error.find("skrivning") != std::string::npos);
}

static void test_rotate_never_applied() {
  RotFixture f;
  f.wlan.writeApplies = false;  // routern säger ok men behåller gamla
  RotateResult r = f.rot.rotate("Old-Pass-22");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(5, f.wlan.reads);
  TEST_ASSERT_TRUE(r.error.find("verifiering") != std::string::npos);
}

static void test_sync() {
  RotFixture f;
  std::string cur, err;
  TEST_ASSERT_TRUE(f.rot.sync(cur, err));
  TEST_ASSERT_EQUAL_STRING("Old-Pass-22", cur.c_str());
  f.wlan.readFailures = 1;
  TEST_ASSERT_FALSE(f.rot.sync(cur, err));
}

// ---------------------------------------------------------------- schema
static LocalTime at(int ymd, int h, int m) {
  LocalTime t;
  t.year = ymd / 10000;
  t.month = ymd / 100 % 100;
  t.day = ymd % 100;
  t.hour = h;
  t.minute = m;
  return t;
}

static void test_schedule() {
  Schedule s(3, 0, 7);
  TEST_ASSERT_FALSE(s.isDue(at(20261006, 3, 0), 0));          // aldrig roterat -> adoptera
  TEST_ASSERT_FALSE(s.isDue(at(20261006, 2, 59), 20261005));  // före 03:00
  TEST_ASSERT_TRUE(s.isDue(at(20261006, 3, 0), 20261005));
  TEST_ASSERT_FALSE(s.isDue(at(20261006, 3, 5), 20261006));   // redan gjort i dag
  TEST_ASSERT_TRUE(s.isDue(at(20261006, 6, 59), 20261003));   // ta igen missad natt
  TEST_ASSERT_FALSE(s.isDue(at(20261006, 7, 0), 20261005));   // för sent – vänta till i natt
  TEST_ASSERT_TRUE(s.isDue(at(20270101, 3, 0), 20261231));    // årsskifte
  TEST_ASSERT_EQUAL_UINT32(60, Schedule::retryDelaySeconds(1));
  TEST_ASSERT_EQUAL_UINT32(900, Schedule::retryDelaySeconds(9));
}

// ---------------------------------------------------------------- webb
static void test_web_content() {
  PublicStatus st{"VidebergKraft_Guest", "Sunny-Tiger-47", true, "2026-10-06T03:00:12+02:00", 3};
  std::string page = renderPage("<b>{{SSID}}</b><i>{{PASSWORD}}</i>{{PASSWORD}}", st);
  TEST_ASSERT_EQUAL_STRING("<b>VidebergKraft_Guest</b><i>Sunny-Tiger-47</i>Sunny-Tiger-47",
                           page.c_str());
  st.password = "<x>&\"";
  TEST_ASSERT_EQUAL_STRING("&lt;x&gt;&amp;&quot;", renderPage("{{PASSWORD}}", st).c_str());

  JsonDocument d;
  deserializeJson(d, statusJson(st));
  TEST_ASSERT_EQUAL_STRING("<x>&\"", d["password"] | "");
  TEST_ASSERT_EQUAL(3, d["version"].as<long>());
  TEST_ASSERT_TRUE(d["verified"].as<bool>());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wordlist_is_clean);
  RUN_TEST(test_password_format);
  RUN_TEST(test_password_never_repeats_previous);
  RUN_TEST(test_uniform_rejects_biased_values);
  RUN_TEST(test_password_distribution_roughly_uniform);
  RUN_TEST(test_parse_content_length_and_cookies);
  RUN_TEST(test_parse_chunked);
  RUN_TEST(test_parse_rejects_garbage);
  RUN_TEST(test_serialize_request);
  RUN_TEST(test_unifi_read_and_write);
  RUN_TEST(test_unifi_relogin_on_expired_session);
  RUN_TEST(test_unifi_follows_rotated_csrf);
  RUN_TEST(test_unifi_errors);
  RUN_TEST(test_rotate_success);
  RUN_TEST(test_rotate_waits_for_router_to_apply);
  RUN_TEST(test_rotate_lost_response_but_applied);
  RUN_TEST(test_rotate_write_failed);
  RUN_TEST(test_rotate_never_applied);
  RUN_TEST(test_sync);
  RUN_TEST(test_schedule);
  RUN_TEST(test_web_content);
  return UNITY_END();
}
