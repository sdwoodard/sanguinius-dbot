#ifndef HELPCOMMAND_HPP_
#define HELPCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class helpCommand : public ICommand
{
public:

  helpCommand() = default;

  ~helpCommand() = default;

  void registerCommand(std::shared_ptr<dpp::cluster> bot) override;

  void commandCallBack(std::string command, const dpp::slashcommand_t& event) override;

private:

  void executeCommand(const dpp::slashcommand_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* HELPCOMMAND_HPP_ */
