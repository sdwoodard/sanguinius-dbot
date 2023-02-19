#ifndef HELPCOMMAND_HPP_
#define HELPCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class helpCommand
{
public:

  helpCommand() = default;

  ~helpCommand() = default;

  void registerCommand(dpp::cluster* bot);

  void commandCallBack(std::string command, const dpp::slashcommand_t& event);

private:

  void executeCommand(const dpp::slashcommand_t& event);

  dpp::cluster* bot;

};

#endif /* HELPCOMMAND_HPP_ */
