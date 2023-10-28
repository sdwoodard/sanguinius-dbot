#ifndef TCREPOCOMMAND_HPP_
#define TCREPOCOMMAND_HPP_

#include <dpp/dpp.h>
#include <tcCommand.hpp>

class tcRepoCommand : public tcCommand
{
public:

  tcRepoCommand() = default;

  ~tcRepoCommand() = default;

  void CommandCallBack(const dpp::message_create_t& arcEvent);

private:

  dpp::cluster* mpcBot;

};

#endif /* TCREPOCOMMAND_HPP_ */
