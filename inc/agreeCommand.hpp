#ifndef AGREECOMMAND_HPP_
#define AGREECOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class agreeCommand
{
public:

  agreeCommand(dpp::cluster* acBot);

  ~agreeCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event, dpp::message& oldMsg);

private:

  void executeCommand(const dpp::message_create_t& event, dpp::message& oldMsg);

  dpp::cluster* bot;

};

#endif /* AGREECOMMAND_HPP_ */
