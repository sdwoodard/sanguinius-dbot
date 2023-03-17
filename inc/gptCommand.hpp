#ifndef GPTCOMMAND_HPP_
#define GPTCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class gptCommand : public ICommand
{
public:

  gptCommand(dpp::cluster* acBot);

  ~gptCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

  std::string mcApiKey;

};

#endif /* GPTCOMMAND_HPP_ */
