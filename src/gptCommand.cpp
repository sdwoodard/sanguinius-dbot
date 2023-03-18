#include <dpp/dpp.h>
#include <gptCommand.hpp>
#include <curl/curl.h>
#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <fstream>
#include <sstream>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

gptCommand::gptCommand(dpp::cluster* acBot)
:
  bot(acBot)
{
  std::ifstream file("/home/sigmar/.secrets/openai.key");
  std::getline(file, mcApiKey);
}

bool gptCommand::commandCallBack(std::string keyword, const dpp::message_create_t& event)
{

  if (keyword == "!gpt")
  {
    this->executeCommand(event);
    return true;
  }
  else
  {
    return false;
  }

}

void gptCommand::executeCommand(const dpp::message_create_t& event)
{
  std::string prompt = event.msg.content.substr(5);

  rapidjson::Document requestBody;
  requestBody.SetObject();
  rapidjson::Document::AllocatorType& allocator = requestBody.GetAllocator();

  requestBody.AddMember("model", "gpt-3.5-turbo", allocator);

  rapidjson::Value messages(rapidjson::kArrayType);
  rapidjson::Value systemMessage(rapidjson::kObjectType);
  systemMessage.AddMember("role", "system", allocator);
  systemMessage.AddMember("content", "You are an AI language model trained to answer questions and engage in conversation.", allocator);
  messages.PushBack(systemMessage, allocator);

  rapidjson::Value userMessage(rapidjson::kObjectType);
  userMessage.AddMember("role", "user", allocator);
  userMessage.AddMember("content", rapidjson::Value().SetString(prompt.c_str(), prompt.length(), allocator), allocator);
  messages.PushBack(userMessage, allocator);
  requestBody.AddMember("messages", messages, allocator);

  requestBody.AddMember("max_tokens", 100, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  requestBody.Accept(writer);

  std::string jsonRequest = buffer.GetString();

  std::stringstream lcResponse;
  try {
    curlpp::Cleanup cleaner;
    curlpp::Easy request;

    std::list<std::string> headers;
    headers.push_back("Content-Type: application/json");
    headers.push_back("Authorization: Bearer " + mcApiKey);

    request.setOpt(new curlpp::options::HttpHeader(headers));
    request.setOpt(new curlpp::options::Url("https://api.openai.com/v1/chat/completions"));
    request.setOpt(new curlpp::options::PostFields(jsonRequest));
    request.setOpt(new curlpp::options::PostFieldSize(jsonRequest.length()));

    std::stringstream responseStream;
    request.setOpt(new curlpp::options::WriteStream(&responseStream));

    request.perform();

    std::string responseStr = responseStream.str();

    std::cout << "Raw API Response: " << responseStr << std::endl;

    rapidjson::Document jsonResponse;
    jsonResponse.Parse(responseStr.c_str());

    if (jsonResponse.HasParseError())
    {
      lcResponse << "Hey bud, I had a parsing error. Sorry.";
      return;
    }

    if (jsonResponse.HasMember("choices") && jsonResponse["choices"].IsArray() && jsonResponse["choices"].Size() > 0)
    {
      const auto& choice = jsonResponse["choices"][0];
      if (choice.HasMember("text") && choice["text"].IsString())
      {
        lcResponse << choice["text"].GetString();
      }
      else
      {
        lcResponse << "Hey bud, I got an unexpected JSON structure back. Sorry.";
      }
    }
    else
    {
      lcResponse << "Hey bud, chatGPT didn't provide a completion back. Sorry.";
    }

  }
  catch (const curlpp::RuntimeError& e)
  {
    lcResponse << "ERROR: cURL runtime error: " << std::string(e.what());
  }
  catch (const curlpp::LogicError& e)
  {
    lcResponse << "ERROR: cURL logic error: " << std::string(e.what());
  }
  catch (const std::exception& e)
  {
    lcResponse << "ERROR: Exception occured: " << std::string(e.what());
  }

  dpp::message response = dpp::message()
    .set_content(lcResponse.str())
    .set_channel_id(event.msg.channel_id);
  bot->message_create(response);

}
