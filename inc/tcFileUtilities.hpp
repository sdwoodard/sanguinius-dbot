#ifndef TCFILEUTILITIES_HPP_
#define TCFILEUTILITIES_HPP_

#include <string>

class tcFileUtilities
{
public:
  // used to read file contents and load it into memory as a string
  static std::string ReadStringFromFile(const std::string &arcFilePath);

  // some lines contain a line/carriage return - this will remove it from string
  static void TrimLineReturn(std::string &arcInputString);

};

#endif /* TCFILEUTILITIES_HPP_ */
