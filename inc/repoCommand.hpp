#ifndef REPOCOMMAND_HPP_
#define REPOCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class repoCommand : public ICommand
{
public:

  repoCommand() = default;

  ~repoCommand() = default;

  void registerCommand(std::shared_ptr<dpp::cluster> bot);

  void commandCallBack(std::string command, const dpp::message_create_t& event);

private:

  void executeCommand(const dpp::message_create_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* REPOCOMMAND_HPP_ */
