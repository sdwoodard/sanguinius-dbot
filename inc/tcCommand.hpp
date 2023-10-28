#ifndef TCCOMMAND_HPP_
#define TCCOMMAND_HPP_

#include <dpp/dpp.h>

#include <string>

// pure abstract class from which all commands must inherit from
class tcCommand
{
public:

  virtual ~tcCommand() = default;

  // must have common CommandCallBack
  virtual void CommandCallBack(const dpp::message_create_t&) = 0;
};

#endif /* TCCOMMAND_HPP_ */
