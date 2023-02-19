#ifndef VOTECOMMAND_HPP_
#define VOTECOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class voteCommand : public ICommand
{
public:

  voteCommand() = default;

  ~voteCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event);

private:

  void executeCommand(const dpp::message_create_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* VOTECOMMAND_HPP_ */
