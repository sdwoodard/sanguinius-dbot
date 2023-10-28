#ifndef TCJOINCOMMAND_HPP_
#define TCJOINCOMMAND_HPP_

#include <dpp/dpp.h>
#include <tcCommand.hpp>

class tcJoinCommand : public tcCommand
{
public:

  tcJoinCommand() = default;

  ~tcJoinCommand() = default;

  void CommandCallBack(const dpp::message_create_t& arcEvent) override;

private:

  dpp::cluster* mpcBot;

};

#endif /* TCJOINCOMMAND_HPP_ */
