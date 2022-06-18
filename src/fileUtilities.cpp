#include <fileUtilities.hpp>

#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>

std::string fileUtilities::readStringFromFile(const std::string &filePath)
{
  const std::ifstream inputStream(filePath, std::ios_base::binary);
  if (inputStream.fail())
    {
      throw std::runtime_error("Failed to open file");
    }

  std::stringstream buffer;
  buffer << inputStream.rdbuf();
  std::string fileString=buffer.str();
  fileUtilities::trimLineReturn(fileString);

  return fileString;
}


bool fileUtilities::trimLineReturn(std::string &inputString)
{
  inputString.erase(std::remove_if
    (
      inputString.begin(),
      inputString.end(),
      [](auto ch) {return (ch == '\n' || ch == '\r');}
    ),
    inputString.end()
  );

  return true;
}
