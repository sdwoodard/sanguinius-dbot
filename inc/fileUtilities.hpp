#ifndef FILEUTILITIES_HPP_
#define FILEUTILITIES_HPP_

#include <string>

class fileUtilities
{
public:

  fileUtilities() = default;

  ~fileUtilities() = default;

  static std::string readStringFromFile(const std::string &filePath);

  static bool trimLineReturn(std::string &inputString);

};

#endif /* FILEUTILITIES_HPP_ */
