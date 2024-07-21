#include "config.h"
#include "json.h"
#include "hex.h"
#include "patch.h"
#include <ws2tcpip.h>

LPCSTR RugburnConfigFilename = "rugburn.json";
RUGBURNCONFIG Config;
const char ExampleRugburnConfig[] = "{\r\n" \
"  \"UrlRewrites\": {\r\n" \
"    \"http://[a-zA-Z0-9:.]+/(.*)\": \"http://localhost:8080/$0\"\r\n" \
"  },\r\n" \
"  \"PortRewrites\": [\r\n" \
"    {\r\n" \
"      \"FromPort\": 10103,\r\n" \
"      \"ToPort\": 10101,\r\n" \
"      \"ToAddr\": \"localhost\"\r\n" \
"    }\r\n" \
"  ]\r\n" \
"}\r\n";

void ReadJsonUrlRewriteRuleMap(LPSTR *json, LPCSTR key) {
    LPCSTR value = JsonReadString(json);

    if (Config.NumUrlRewriteRules == MAXURLREWRITES) {
        FatalError("Reached maximum number of URL rewrite rules!");
    }

    Config.UrlRewriteRules[Config.NumUrlRewriteRules].from = ReParse(key);
    Config.UrlRewriteRules[Config.NumUrlRewriteRules].to = value;
    Config.NumUrlRewriteRules++;
}

void ReadJsonPortRewriteRuleMap(LPSTR *json, LPCSTR key) {
    if (!strcmp(key, "FromPort")) {
        Config.PortRewriteRules[Config.NumPortRewriteRules].fromport = JsonReadInteger(json);
    } else if (!strcmp(key, "ToPort")) {
        Config.PortRewriteRules[Config.NumPortRewriteRules].toport = JsonReadInteger(json);
    } else if (!strcmp(key, "ToAddr")) {
        Config.PortRewriteRules[Config.NumPortRewriteRules].toaddr = JsonReadString(json);
    } else {
        FatalError("Unexpected JSON config key in port rewrite rule: '%s'", key);
    }
}

void ReadJsonPortRewriteRuleArray(LPSTR *json) {
    if (Config.NumPortRewriteRules == MAXURLREWRITES) {
        FatalError("Reached maximum number of URL rewrite rules!");
    }

    JsonReadMap(json, ReadJsonPortRewriteRuleMap);
    Config.NumPortRewriteRules++;
}

void ReadJsonPatchAddressMap(LPSTR *json, LPCSTR key) {
	LPCSTR value = JsonReadString(json);

	if (Config.NumPatchAddress == MAXPATCHADDRESS) {
		FatalError("Reached maximum number of Patch address!");
	}

	Config.PatchAddress[Config.NumPatchAddress].addr = ParseAddress(key);
	ParsePatch(value, &Config.PatchAddress[Config.NumPatchAddress].patch, &Config.PatchAddress[Config.NumPatchAddress].patchLen);
	Config.NumPatchAddress++;
}

void ReadJsonConfigMap(LPSTR *json, LPCSTR key) {
    if (!strcmp(key, "UrlRewrites")) {
        JsonReadMap(json, ReadJsonUrlRewriteRuleMap);
    } else if (!strcmp(key, "PortRewrites")) {
        JsonReadArray(json, ReadJsonPortRewriteRuleArray);
    } else if (!strcmp(key, "PatchAddress")) {
		JsonReadMap(json, ReadJsonPatchAddressMap);
	} else {
        FatalError("Unexpected JSON config key '%s'", key);
    }
}

void LoadJsonRugburnConfig() {
    LPSTR json;
    if (!FileExists(RugburnConfigFilename)) {
        Warning("No rugburn.json config file found. An example configuration will be saved.");
        WriteEntireFile(RugburnConfigFilename, ExampleRugburnConfig, sizeof(ExampleRugburnConfig) - 1);
        json = DupStr(ExampleRugburnConfig);
    } else {
        json = ReadEntireFile(RugburnConfigFilename);
    }
    memset(&Config, 0, sizeof(RUGBURNCONFIG));
    JsonReadMap(&json, ReadJsonConfigMap);
}

LPCSTR RewriteURL(LPCSTR url) {
    LPCSTR result;
    int i;

    for (i = 0; i < Config.NumUrlRewriteRules; i++) {
        result = ReReplace(Config.UrlRewriteRules[i].from, Config.UrlRewriteRules[i].to, url);
        if (result != NULL) {
            return result;
        }
    }
    return NULL;
}

BOOL RewriteAddr(LPSOCKADDR_IN addr) {
    ADDRINFOA hints;
    ADDRINFOA *resolved, *ptr;
    DWORD result;
    int i;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    for (i = 0; i < Config.NumPortRewriteRules; i++) {
        if (addr->sin_port == pHtons(Config.PortRewriteRules[i].fromport)) {
            result = pGetAddrInfo(Config.PortRewriteRules[i].toaddr, NULL, &hints, &resolved);
            if (result != 0) {
                Log("Warning: failed to resolve %s (result=%08x)\r\n", Config.PortRewriteRules[i].toaddr, result);
                continue;
            }
            ptr = resolved;
            do {
                if (ptr->ai_family != AF_INET) {
                    Log("Skipping result because it is for a different address family (%i)", ptr->ai_family);
                }
                break;
            } while((ptr = ptr->ai_next));
            if (!ptr) {
                Log("Warning: no suitable result for %s.", Config.PortRewriteRules[i].toaddr);
                continue;
            }
            memcpy(addr, resolved->ai_addr, sizeof(struct sockaddr_in));
            addr->sin_port = pHtons(Config.PortRewriteRules[i].toport);
            pFreeAddrInfo(resolved);
            return TRUE;
        }
    }

    return FALSE;
}

void PatchAddress() {
    int i;
    MEMORY_BASIC_INFORMATION mbi;
    for (i = 0; i < Config.NumPatchAddress; i++) {
        if (Config.PatchAddress[i].addr == 0) {
            Warning("Patch %d at address 0 will be ignored.", i);
            continue;
        }
        if (Config.PatchAddress[i].patchLen == 0) {
            Warning("Patch %d is empty.", i);
            continue;
        }
		if (pVirtualQuery((void*)Config.PatchAddress[i].addr, &mbi, sizeof(mbi)) == 0) {
			Log("PatchAddress 0x%08lX failed in VirtualQuery, ErrorCode: %08lX\r\n", Config.PatchAddress[i].addr, pGetLastError());
			continue;
		}
		if (mbi.Type != MEM_IMAGE) {
			Log("PatchAddress 0x%08lX is not image memory.\r\n", Config.PatchAddress[i].addr);
			continue;
		}
        Patch((LPVOID)Config.PatchAddress[i].addr, Config.PatchAddress[i].patch, Config.PatchAddress[i].patchLen);
        Log("PatchAddress: 0x%08lX, Len: %d, Value: %s\r\n", Config.PatchAddress[i].addr, Config.PatchAddress[i].patchLen, Config.PatchAddress[i].patch);
    }
}
