// SMB (Windows / Samba / NAS) network shares.
#pragma once

#include <string>
#include <vector>

namespace smb {

struct Share {
    std::string name;     // display name
    std::string host;     // hostname or IP
    std::string share;    // share name ("Media")
    std::string user, password, domain;
};

std::vector<Share> saved_shares();
void save_share(const Share& s);
void remove_share(const std::string& name);

}  // namespace smb
