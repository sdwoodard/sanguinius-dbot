#ifndef REPOCOMMAND_HPP_
#define REPOCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class repoCommand : public ICommand
{
public:

  repoCommand() = default;

  ~repoCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event);

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

};

#endif /* REPOCOMMAND_HPP_ */
