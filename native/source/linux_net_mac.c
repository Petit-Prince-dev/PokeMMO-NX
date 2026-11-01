#include "linux_net_native.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include <string.h>
#include <switch.h>

int linuxNetNativeMacAddress(uint8_t address[6], int *error) {
    SetCalMacAddress mac;
    Result rc = setcalInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = setcalGetWirelessLanMacAddress(&mac);
        setcalExit();
    }
    if (R_FAILED(rc)) {
        diagnosticsTrace("net.mac=ERROR rc=0x%x", rc);
        *error = LINUX_EIO;
        return -1;
    }
    uint8_t any = 0;
    for (unsigned i = 0; i < sizeof(mac.addr); ++i) any |= mac.addr[i];
    // An unavailable or invalid hardware address must not become a shared all-zero identity.
    if (!any || (mac.addr[0] & 1)) {
        diagnosticsTrace("net.mac=ERROR invalid_address");
        *error = LINUX_EIO;
        return -1;
    }
    memcpy(address, mac.addr, sizeof(mac.addr));
    return 0;
}
