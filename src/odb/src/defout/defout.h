#pragma once

#include <string>

#include "odb/odb.h"

namespace utl {
class Logger;
}

namespace odb {

class dbBlock;
class dbNet;
class dbInst;

class defout
{
 public:
  enum Version
  {
    DEF_5_8,
    DEF_5_7,
    DEF_5_6,
    DEF_5_5,
    DEF_5_4,
    DEF_5_3
  };

  defout(utl::Logger* logger);
  ~defout();

  void setVersion(Version v);
  void setUseLayerAlias(bool value);
  void setUseNetInstIds(bool value);
  void setUseMasterIds(bool value);
  void selectNet(dbNet* net);
  bool writeBlock(dbBlock* block, const char* def_file);
  bool writeBlock_Pl(dbBlock* block, const char* def_file);

 private:
  class defout_impl* _writer;
};

}  // namespace odb 