#ifndef JOINCOMMAND_HPP_
#define JOINCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class joinCommand : public ICommand
{
public:

  joinCommand() = default;

  ~joinCommand() = default;

  void registerCommand(std::shared_ptr<dpp::cluster> bot) override;

  void commandCallBack(std::string command, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* JOINCOMMAND_HPP_ */
