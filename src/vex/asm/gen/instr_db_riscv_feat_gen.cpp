// GENERADO por tools/import/gen_cpp_db.py -- NO editar a mano.
// Features (extensiones de ISA) por CPU riscv.
#include "vex/asm/instr_db.h"

namespace vex { namespace instr_db { namespace {

const char *const kFn[] = {"32Bit","64Bit","StdExtA","StdExtC","StdExtD","StdExtF","StdExtI","StdExtM","StdExtSsccptr","StdExtSscofpmf","StdExtSscounterenw","StdExtSstc","StdExtSstvala","StdExtSstvecd","StdExtSvade","StdExtSvbare","StdExtSvinval","StdExtSvnapot","StdExtSvpbmt","StdExtV","StdExtZa64rs","StdExtZba","StdExtZbb","StdExtZbc","StdExtZbkb","StdExtZbkc","StdExtZbkx","StdExtZbs","StdExtZfh","StdExtZfhmin","StdExtZic64b","StdExtZicbom","StdExtZicbop","StdExtZicboz","StdExtZiccamoa","StdExtZiccif","StdExtZicclsm","StdExtZiccrse","StdExtZicntr","StdExtZicond","StdExtZicsr","StdExtZifencei","StdExtZihintntl","StdExtZihintpause","StdExtZihpm","StdExtZkn","StdExtZknd","StdExtZkne","StdExtZknh","StdExtZksed","StdExtZksh","StdExtZkt","StdExtZmmul","StdExtZvbb","StdExtZvbc","StdExtZve32f","StdExtZve32x","StdExtZve64d","StdExtZve64f","StdExtZve64x","StdExtZvfh","StdExtZvfhmin","StdExtZvkb","StdExtZvkg","StdExtZvkn","StdExtZvknc","StdExtZvkned","StdExtZvkng","StdExtZvknhb","StdExtZvks","StdExtZvksc","StdExtZvksed","StdExtZvksg","StdExtZvksh","StdExtZvkt","StdExtZvl128b","StdExtZvl256b","StdExtZvl32b","StdExtZvl512b","StdExtZvl64b","UnalignedScalarMem","UnalignedVectorMem","VendorXVentanaCondOps"};
const uint16_t kCf0[] = {0,6};
const uint16_t kCf1[] = {1,6};
const uint16_t kCf2[] = {0,6,40,41};
const uint16_t kCf3[] = {1,6,40,41};
const uint16_t kCf4[] = {0,3,6,7,40,41,52};
const uint16_t kCf5[] = {0,2,3,6,7,40,41,52};
const uint16_t kCf6[] = {0,2,3,5,6,7,40,41,52};
const uint16_t kCf7[] = {0,2,3,6,7,40,41,52};
const uint16_t kCf8[] = {0,2,3,5,6,7,40,41,52};
const uint16_t kCf9[] = {0,2,3,5,6,7,40,41,52};
const uint16_t kCf10[] = {1,2,3,4,5,6,7,20,21,22,27,29,30,31,32,33,34,35,36,37,40,41,42,43,44,52,80,81};
const uint16_t kCf11[] = {1,2,3,4,5,6,7,19,20,21,22,27,29,30,31,32,33,34,35,36,37,40,41,42,43,44,52,53,54,55,56,57,58,59,62,63,64,65,66,67,68,69,70,71,72,73,74,75,77,79,80,81};
const uint16_t kCf12[] = {1,2,3,6,7,40,41,52};
const uint16_t kCf13[] = {1,2,3,6,7,40,41,52};
const uint16_t kCf14[] = {1,2,3,4,5,6,7,40,41,52};
const uint16_t kCf15[] = {1,2,3,4,5,6,7,40,41,43,52};
const uint16_t kCf16[] = {1,2,3,4,5,6,7,40,41,52};
const uint16_t kCf17[] = {1,2,3,4,5,6,7,40,41,52};
const uint16_t kCf18[] = {1,2,3,4,5,6,7,19,21,22,28,29,40,41,52,55,56,57,58,59,60,61,75,76,77,78,79};
const uint16_t kCf19[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,25,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,43,44,51,52,55,56,57,58,59,60,61,74,75,76,77,79};
const uint16_t kCf20[] = {0,3,6,40,41};
const uint16_t kCf21[] = {0,3,6,7,40,41,52};
const uint16_t kCf22[] = {0,3,6,7,40,41,52};
const uint16_t kCf23[] = {1,2,3,6,7,40,41,52};
const uint16_t kCf24[] = {1,2,3,4,5,6,7,21,22,23,27,31,32,33,38,40,41,43,44,52,82};
const uint16_t kCf25[] = {1,2,3,4,5,6,7,16,21,22,23,24,25,26,27,31,33,40,41,45,46,47,48,49,50,52};
const CpuFeatures kCpu[] = {
  {"generic-rv32", "NoSchedModel", kCf0, 2},
  {"generic-rv64", "NoSchedModel", kCf1, 2},
  {"rocket-rv32", "RocketModel", kCf2, 4},
  {"rocket-rv64", "RocketModel", kCf3, 4},
  {"sifive-e20", "RocketModel", kCf4, 7},
  {"sifive-e21", "RocketModel", kCf5, 8},
  {"sifive-e24", "RocketModel", kCf6, 9},
  {"sifive-e31", "RocketModel", kCf7, 8},
  {"sifive-e34", "RocketModel", kCf8, 9},
  {"sifive-e76", "SiFive7Model", kCf9, 9},
  {"sifive-p450", "SiFiveP400Model", kCf10, 28},
  {"sifive-p670", "SiFiveP600Model", kCf11, 52},
  {"sifive-s21", "RocketModel", kCf12, 8},
  {"sifive-s51", "RocketModel", kCf13, 8},
  {"sifive-s54", "RocketModel", kCf14, 10},
  {"sifive-s76", "SiFive7Model", kCf15, 11},
  {"sifive-u54", "RocketModel", kCf16, 10},
  {"sifive-u74", "SiFive7Model", kCf17, 10},
  {"sifive-x280", "SiFive7Model", kCf18, 27},
  {"spacemit-x60", "NoSchedModel", kCf19, 55},
  {"syntacore-scr1-base", "SyntacoreSCR1Model", kCf20, 5},
  {"syntacore-scr1-max", "SyntacoreSCR1Model", kCf21, 7},
  {"syntacore-scr3-rv32", "SyntacoreSCR3RV32Model", kCf22, 7},
  {"syntacore-scr3-rv64", "SyntacoreSCR3RV64Model", kCf23, 8},
  {"veyron-v1", "NoSchedModel", kCf24, 21},
  {"xiangshan-nanhu", "XiangShanNanHuModel", kCf25, 26},
};

} // namespace anonimo

const FeatData &feat_riscv() {
  static const FeatData d = {kFn, 83, kCpu, 26};
  return d;
}

}} // namespace vex::instr_db
