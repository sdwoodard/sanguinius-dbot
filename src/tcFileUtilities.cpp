#include <tcFileUtilities.hpp>

#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <stdexcept>

std::string tcFileUtilities::ReadStringFromFile(const std::string &arcFilePath)
{
  const std::ifstream inputStream(arcFilePath, std::ios_base::binary);
  if (inputStream.fail())
    {
      throw std::runtime_error("Failed to open file: " + arcFilePath);
    }

  std::stringstream buffer;
  buffer << inputStream.rdbuf();
  std::string fileString=buffer.str();
  tcFileUtilities::TrimLineReturn(fileString);

  return fileString;
}


void tcFileUtilities::TrimLineReturn(std::string &arcInputString)
{
  arcInputString.erase(std::remove_if
    (
      arcInputString.begin(),
      arcInputString.end(),
      [](auto ch) {return (ch == '\n' || ch == '\r');}
    ),
    arcInputString.end()
  );
}
