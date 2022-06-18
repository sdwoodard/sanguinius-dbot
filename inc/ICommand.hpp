#ifndef ICOMMAND_HPP_
#define ICOMMAND_HPP_

#include <dpp/dpp.h>

#include <string>

class ICommand
{
public:

  virtual ~ICommand() = default;

  virtual void registerCommand(std::shared_ptr<dpp::cluster>) = 0;

  virtual void commandCallBack(std::string command, const dpp::slashcommand_t&) = 0;
};

#endif /* ICOMMAND_HPP_ */
