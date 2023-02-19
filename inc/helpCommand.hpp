#ifndef HELPCOMMAND_HPP_
#define HELPCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class helpCommand : public ICommand
{
public:

  helpCommand() = default;

  ~helpCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

};

#endif /* HELPCOMMAND_HPP_ */
