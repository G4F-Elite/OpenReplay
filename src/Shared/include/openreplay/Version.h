#pragma once

#include <string_view>

namespace openreplay {

inline constexpr std::string_view kProductName{"OpenReplay"};
inline constexpr std::string_view kVersion{"0.1.1"};
inline constexpr std::string_view kReleaseChannel{"stable"};
inline constexpr std::string_view kRepositoryUrl{"https://github.com/G4F-Elite/OpenReplay"};
inline constexpr std::string_view kUpdateManifestUrl{
    "https://github.com/G4F-Elite/OpenReplay/releases/latest/download/OpenReplay-update.json"};
inline constexpr std::string_view kUpdateSignatureUrl{
    "https://github.com/G4F-Elite/OpenReplay/releases/latest/download/OpenReplay-update.json.sig"};

// RSA-3072 public key used only to verify release metadata. The private key is
// stored as a GitHub Actions secret and is never included in the repository.
inline constexpr std::string_view kUpdatePublicKeyBase64{
    "UlNBMQAMAAADAAAAgAEAAAAAAAAAAAAAAQABzHcNcO9nTPSyC/qMDhJ45HVxa9qnJ3G1MBKJboKob6B9jcSYoyLz5I9FjiOvFTPS2DH3xMDjzAuba2PCAe2crdsuyflnaxgvE57jg20+Hq0Wr/z0hxmxmsoCBYmzkqKV5ytI4oSYLWfzHXdNIh+d6cG+a4ezFPRnI2CWNkFMw1A8EPcn2qsLGbeOuxJlnYVlIKkkyRGz/53AlVBcWBP1hKGgNMhjyRVunPUrHNcThAYxtQiFdyYX/JUbNTAKXTFYq+iW2juBM3LKevKMp6Cs2cgKzsvlSWY65brN41o8IrJCuGPS1fm16p7gqso/tDWsPVWUD0X9KZKsdglj/Qonh/hLfQfvl9gmhAlKHbHcQZxjnpwdDOIdFrtf8qM5H4UTlwIiS+F34ewRkFHZuDkZSmGC1EXaB1UN7+Un6t3F4xTXn9+klVVVgztUWrmFASoQvERo+c6VTXXqgal4vPpXwFKTmm0TYTnETnmsF95WdMJP2HLaQUzKXpFfKquckdzd"};

}  // namespace openreplay
