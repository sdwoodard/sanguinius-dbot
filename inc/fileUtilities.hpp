#ifndef FILEUTILITIES_HPP_
#define FILEUTILITIES_HPP_

#include <string>

class fileUtilities
{
public:

  fileUtilities() = default;

  ~fileUtilities() = default;

  // used to read file contents and load it into memory as a string
  static std::string readStringFromFile(const std::string &filePath);

  // some lines contain a line/carriage return - this will remove it from string
  static bool trimLineReturn(std::string &inputString);

};

#endif /* FILEUTILITIES_HPP_ */
