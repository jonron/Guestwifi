#pragma once
// Kopiera till include/secrets.h och fyll i. secrets.h är gitignorerad.

#define SECRET_WIFI_PASSWORD   "lösenord-till-VidebergKraft_IOT"

// Lokalt UniFi-konto (enbart lokal åtkomst, utan MFA) med rätt att ändra WiFi.
#define SECRET_UDM_USER        ""
#define SECRET_UDM_PASS        ""

// SHA-256-fingeravtryck för UDM:ens certifikat. Hämta med:
//   openssl s_client -connect 192.168.40.1:443 </dev/null 2>/dev/null \
//     | openssl x509 -noout -fingerprint -sha256
// Tom sträng = ingen certifikatkontroll (endast för test!).
#define SECRET_UDM_CERT_SHA256 ""

// Token för manuell rotation: curl -X POST -H "Authorization: Bearer <token>" http://192.168.40.6/api/rotate
#define SECRET_ADMIN_TOKEN     "byt-mig"
