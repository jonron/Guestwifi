// Kör firmwarens kärnlogik på datorn mot mock_udm.py över riktig HTTP.
// Byggs och körs av sim/run_sim.sh.
#include <GuestCore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

using namespace guest;

// Klartext-HTTP mot 127.0.0.1 (på ESP:n är det HTTPS via WiFiClientSecure).
class PosixHttpTransport : public HttpTransport {
 public:
  explicit PosixHttpTransport(int port) : port_(port) {}
  bool send(const HttpRequest& req, HttpResponse& resp, std::string& error) override {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      error = "kan inte ansluta till UDM";
      return false;
    }
    std::string raw = serializeRequest(req, "192.168.40.1");
    ::send(fd, raw.data(), raw.size(), 0);
    std::string in;
    char buf[1024];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) in.append(buf, n);
    close(fd);
    if (in.empty()) {
      error = "inget svar (anslutningen stängdes)";
      return false;
    }
    return parseResponse(in, resp, error);
  }

 private:
  int port_;
};

static int gPort = 8443;
static int gFailed = 0;

static void simControl(const std::string& path, const std::string& body) {
  PosixHttpTransport t(gPort);
  HttpResponse r;
  std::string err;
  t.send({"POST", path, {}, body}, r, err);
}

static void check(bool cond, const char* what) {
  std::printf("  %s %s\n", cond ? "OK  " : "FEL ", what);
  if (!cond) gFailed++;
}

// Samma flöde som firmwarens rotationstråd, men med virtuell tid.
struct Device {
  PosixHttpTransport transport;
  UnifiClient unifi;
  std::mt19937 rng{std::random_device{}()};
  PasswordGenerator gen{[this] { return static_cast<uint32_t>(rng()); }};
  Rotator rotator{unifi, gen, "VidebergKraft_Guest", [](uint32_t) { usleep(20000); }, 12, 0};
  Schedule schedule{3, 0, 7};
  PublicStatus status;
  int lastRotatedYmd = 0;

  Device(int port, const char* user, const char* pass) : transport(port), unifi(transport, user, pass) {
    status.ssid = "VidebergKraft_Guest";
  }

  void publish(const std::string& pw, const std::string& when) {
    if (pw != status.password || !status.verified) {
      status.password = pw;
      status.verified = true;
      status.updated = when;
      status.version++;
    }
  }

  bool boot(const LocalTime& now) {
    std::string cur, err;
    if (!rotator.sync(cur, err)) {
      std::printf("  sync misslyckades: %s\n", err.c_str());
      return false;
    }
    publish(cur, "boot");
    if (lastRotatedYmd == 0) lastRotatedYmd = now.ymd();
    return true;
  }

  RotateResult tick(const LocalTime& now) {
    RotateResult r;
    if (!schedule.isDue(now, lastRotatedYmd)) {
      r.error = "inte dags";
      return r;
    }
    r = rotator.rotate(status.password);
    if (r.ok) {
      char when[32];
      std::snprintf(when, sizeof(when), "%04d-%02d-%02dT%02d:%02d:00+0200", now.year, now.month,
                    now.day, now.hour, now.minute);
      publish(r.password, when);
      lastRotatedYmd = now.ymd();
    }
    return r;
  }
};

static LocalTime at(int y, int mo, int d, int h, int mi) {
  LocalTime t;
  t.year = y, t.month = mo, t.day = d, t.hour = h, t.minute = mi;
  return t;
}

static std::string readFile(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc > 1) gPort = std::atoi(argv[1]);
  const std::string outDir = argc > 2 ? argv[2] : "sim/out";
  simControl("/__sim/reset", "{}");

  Device dev(gPort, "sim-admin", "sim-password");

  std::puts("\n1. Första start 14:10 – adopterar routerns lösenord, ingen rotation");
  check(dev.boot(at(2026, 10, 6, 14, 10)), "läste lösenordet från UDM");
  check(dev.status.password == "Start-Pass-22", "skyltsidan visar Start-Pass-22");
  check(!dev.tick(at(2026, 10, 6, 14, 11)).ok, "ingen rotation mitt på dagen");

  std::puts("\n2. Natt 1, 02:59 och 03:00");
  check(!dev.tick(at(2026, 10, 7, 2, 59)).ok, "02:59 – inte dags");
  RotateResult r = dev.tick(at(2026, 10, 7, 3, 0));
  check(r.ok, ("03:00 – roterat till " + r.password).c_str());
  check(dev.status.password == r.password && dev.status.version == 2, "skyltsidan uppdaterad");
  check(!dev.tick(at(2026, 10, 7, 3, 1)).ok, "03:01 – redan gjort i natt");

  std::puts("\n3. Natt 2: sessionen har gått ut + CSRF-token roteras");
  simControl("/__sim/faults", R"({"expire_session":true,"rotate_csrf":true})");
  std::string before = dev.status.password;
  r = dev.tick(at(2026, 10, 8, 3, 0));
  check(r.ok && r.password != before, "loggade in igen och roterade");

  std::puts("\n4. Natt 3: PUT-svaret tappas (AP:n omprovisioneras) – verifieras ändå");
  simControl("/__sim/faults", R"({"drop_put_response":true})");
  r = dev.tick(at(2026, 10, 9, 3, 0));
  check(r.ok, "verifierad trots tappat svar");

  std::puts("\n5. Natt 4: routern tillämpar ändringen först efter 3 läsningar");
  simControl("/__sim/faults", R"({"apply_after_reads":3})");
  r = dev.tick(at(2026, 10, 10, 3, 0));
  check(r.ok, "väntade in routern och verifierade");

  std::puts("\n6. Natt 5: UDM nekar ändringen – gamla lösenordet ska ligga kvar");
  simControl("/__sim/faults", R"({"reject_put":true})");
  before = dev.status.password;
  long ver = dev.status.version;
  r = dev.tick(at(2026, 10, 11, 3, 0));
  check(!r.ok, ("rotation misslyckades: " + r.error).c_str());
  check(dev.status.password == before && dev.status.version == ver, "skyltsidan oförändrad");
  r = dev.tick(at(2026, 10, 11, 3, 1));
  check(r.ok, "nytt försök lyckas");

  std::puts("\n7. Missad natt (strömavbrott): tas igen 06:30, men inte 07:00");
  r = dev.tick(at(2026, 10, 13, 6, 30));
  check(r.ok, "06:30 – tog igen missad rotation");
  dev.lastRotatedYmd = 20261013;
  check(!dev.tick(at(2026, 10, 14, 7, 0)).ok, "07:00 – väntar till nästa natt");

  std::puts("\n8. UDM nere / fel inloggning");
  {
    Device down(1, "sim-admin", "sim-password");
    check(!down.boot(at(2026, 10, 6, 12, 0)), "UDM onåbar ger fel, ingen krasch");
    Device wrong(gPort, "sim-admin", "fel");
    check(!wrong.boot(at(2026, 10, 6, 12, 0)), "fel lösenord ger fel");
  }

  // Skriv ut samma sida och JSON som ESP:n serverar, för förhandsvisning.
  std::string page = renderPage(readFile("src/web/index.html"), dev.status);
  std::ofstream(outDir + "/index.html") << page;
  std::ofstream(outDir + "/api/status") << statusJson(dev.status);
  std::printf("\nSlutligt lösenord på skylten: %s (version %ld)\n", dev.status.password.c_str(),
              dev.status.version);

  std::printf("\n%s – %d fel\n", gFailed ? "SIMULERING MISSLYCKADES" : "SIMULERING OK", gFailed);
  return gFailed ? 1 : 0;
}
