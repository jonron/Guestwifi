#pragma once
#include <string>

namespace guest {

struct PublicStatus {
  std::string ssid;
  std::string password;   // senast verifierade lösenordet ("" = okänt)
  bool verified = false;  // true när routern bekräftat lösenordet sedan uppstart
  std::string updated;    // ISO-8601, när lösenordet senast byttes eller bekräftades
  long version = 0;       // ökar vid varje byte – skyltsidan ritar om när den ändras
};

std::string htmlEscape(const std::string& s);

// Ersätter {{SSID}} och {{PASSWORD}} i skyltsidan (för spelare utan JavaScript).
std::string renderPage(const std::string& tmpl, const PublicStatus& st);

std::string statusJson(const PublicStatus& st);

// För e-pappersskärmen (reTerminal E1002): samma data plus när den ska vakna nästa gång.
//   next_rotation_s  sekunder till nattens rotation (-1 = okänt, t.ex. tid ej synkad)
//   refresh_s        hur länge skärmen ska sova (till nästa uppdateringstid, t.ex. 04:00)
std::string displayJson(const PublicStatus& st, long nextRotationSeconds, long refreshSeconds);

}  // namespace guest
