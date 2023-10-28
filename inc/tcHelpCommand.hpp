#ifndef TCHELPCOMMAND_HPP_
#define TCHELPCOMMAND_HPP_

#include <dpp/dpp.h>
#include <tcCommand.hpp>

class tcHelpCommand : public tcCommand
{
public:

  tcHelpCommand() = default;

  ~tcHelpCommand() = default;

  void CommandCallBack(const dpp::message_create_t& arcEvent) override;

private:

  dpp::cluster* mpcBot;

};

#endif /* TCHELPCOMMAND_HPP_ */
