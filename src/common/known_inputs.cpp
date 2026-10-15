#include "common/known_inputs.h"

namespace ac4::inputs {

const Known kEbootBin = {"EBOOT.BIN", "96fb034cb52748c76d243d665b1aa4979ba384b7c5deba38b02438ff7de52ce1", 20282920};
const Known kEbootElf = {"EBOOT.elf", "084c9c3fbe6b68e0e3427a056385f50474cdc7d28cabb167bb2c5d1e91c7aa15", 20280488};

const char* const kModules[kModuleCount] = {"libsre",  "libsail", "libpamf",       "libdmux",   "libdmuxpamf",
                                            "libadec", "libvpost", "libatrac3plus", "libatxdec", "libat3dec"};

namespace {

// RPCS3's decryptions of the 4.93 modules (verified 2026-10-15 with RPCS3 0.0.42-19698: identical to
// this project's own decryption; relocated they give the modules data/spu_manifest.txt lists).
const Known kModules493[kModuleCount] = {
    {"libsre.prx", "9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6", 239344},
    {"libsail.prx", "ac33b80980d45113d887c868ff02619dc145e8a80bc102749a31fbd750431bbb", 203400},
    {"libpamf.prx", "363d9681d7f24201b1dfb9cc8714e7182a72a25a57734db2a7e1c3dccad09646", 21336},
    {"libdmux.prx", "2a53cad71a50e2042a7f78ff61675e99deebbdb0be57ab6fdeeaf85b224ca918", 13000},
    {"libdmuxpamf.prx", "792e1fd566717b15a6f5a270cbcb3092ce84abc7f562632fcc1bc580b5960579", 54152},
    {"libadec.prx", "88a8f75b97f59930c88695e4436383300f2112badfa44ea4e6dccad6eda60f61", 65552},
    {"libvpost.prx", "19953feeb9af6858b3cbfaca76bbda1d49ee78216d214ee9cdce7191b361308a", 347552},
    {"libatrac3plus.prx", "85b16a71db983ba298c881d4f439e0930969a1455d07fbebbe59ea4a3df04151", 20792},
    {"libatxdec.prx", "cad06f3a72111aca8f4d6a9c7397310a40a9ef98c703406a75935aaa422f1da1", 183600},
    {"libat3dec.prx", "67f51c3a556fef540ca5da15b1f7560d949640a7607fe4629c3c767264b6ad1b", 52200},
};

// Other versions' decryptions that differ from 4.93 (the other modules are identical there).
struct Other {
  const char* module;
  const char* sha256;
  const char* version;
};
const Other kOtherVersions[] = {
    {"libsre", "74a023767aae35838f26ef1a846806caad438a041a06ba16b1165050aa403e8e", "4.89"},
    {"libsail", "837e2c5a2d53b344b897aa99573ba4c776417d5359970e5a6d23fda9d2fa3441", "4.89"},
    {"libadec", "c3682f6109b1ecf49871b4550464164524dfe39ccea142299d7564565313bee2", "4.89"},
};

}  // namespace

const Known* module(const std::string& name) {
  for (int i = 0; i < kModuleCount; ++i)
    if (name == kModules[i]) return &kModules493[i];
  return nullptr;
}

std::string module_other_version(const std::string& name, const std::string& sha256) {
  for (const Other& o : kOtherVersions)
    if (name == o.module && sha256 == o.sha256) return o.version;
  return "";
}

const Known kFlashAtrac = {"flashATRAC.pic", "1edf54e1323dd736b71958ca56307d536dedc7c05ccb0dd3b2a3be1af5b50e30", 35131};
const Known kRegulation = {"regulation.bin", "c7e993ad592c681963426b7ac7f2c5f4de28aa8b8a1a466f2b1518954232f70f", 245656};

}  // namespace ac4::inputs
