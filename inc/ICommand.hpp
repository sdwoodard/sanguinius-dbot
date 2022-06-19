#ifndef ICOMMAND_HPP_
#define ICOMMAND_HPP_

#include <dpp/dpp.h>

#include <string>

// pure abstract class from which all commands must inherit from
class ICommand
{
public:

  virtual ~ICommand() = default;

  // must have common registerCommand
  virtual void registerCommand(std::shared_ptr<dpp::cluster>) = 0;

  // must have common commandCallBack
  virtual void commandCallBack(std::string command, const dpp::slashcommand_t&) = 0;
};

#endif /* ICOMMAND_HPP_ */
