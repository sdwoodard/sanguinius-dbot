#ifndef JOINCOMMAND_HPP_
#define JOINCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class joinCommand : public ICommand
{
public:

  joinCommand() = default;

  ~joinCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

};

#endif /* JOINCOMMAND_HPP_ */
