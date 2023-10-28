#ifndef TCGPTCOMMAND_HPP_
#define TCGPTCOMMAND_HPP_

#include <dpp/dpp.h>
#include <tcCommand.hpp>

class tcGptCommand : public tcCommand
{
public:

  tcGptCommand(dpp::cluster* apcBot);

  ~tcGptCommand() = default;

  void CommandCallBack(const dpp::message_create_t& arcEvent) override;

private:

  std::string PrepareGptRequestBody(const std::string& arcPrompt);

  std::string MakeGptApiCall(const std::string& arcRequestBody);

  dpp::cluster* mpcBot;

  std::string mcApiKey;

};

#endif /* TCGPTCOMMAND_HPP_ */
