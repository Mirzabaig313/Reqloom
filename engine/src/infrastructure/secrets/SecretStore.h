// Engine-internal alias for the now-public SecretStore interface. Kept so
// existing infrastructure/application includes ("infrastructure/secrets/
// SecretStore.h") resolve unchanged; the definition lives in the public
// header so embedders (desktop secret manager) can manage credentials.
#pragma once

#include <chainapi/engine/SecretStore.h>
