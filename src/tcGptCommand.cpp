#include <tcGptCommand.hpp>

#include <dpp/dpp.h>
#include <curl/curl.h>
#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <fstream>
#include <sstream>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

tcGptCommand::tcGptCommand(dpp::cluster* apcBot)
:
  mpcBot(apcBot)
{
  std::ifstream lcFile("/home/sigmar/.secrets/openai.key");
  std::getline(lcFile, mcApiKey);
}

void tcGptCommand::CommandCallBack(const dpp::message_create_t& arcEvent)
{
  const size_t PROMPT_OFFSET = 5;
  std::string lcPrompt = arcEvent.msg.content.substr(PROMPT_OFFSET);

  std::string lcRequestBody = PrepareGptRequestBody(lcPrompt);
  std::string lcApiResponse = MakeGptApiCall(lcRequestBody);

  dpp::message lcBotResponse = dpp::message()
    .set_content(lcApiResponse)
    .set_channel_id(arcEvent.msg.channel_id);
  
  mpcBot->message_create(lcBotResponse);

}

std::string tcGptCommand::PrepareGptRequestBody(const std::string& arcPrompt)
{
  rapidjson::Document lcRequestBody;
  lcRequestBody.SetObject();
  rapidjson::Document::AllocatorType& lrcAllocator = lcRequestBody.GetAllocator();

  lcRequestBody.AddMember("model", "gpt-3.5-turbo", lrcAllocator);

  rapidjson::Value lcMessages(rapidjson::kArrayType);

  rapidjson::Value lcSystemMessage(rapidjson::kObjectType);
  lcSystemMessage.AddMember("role", "system", lrcAllocator);
  lcSystemMessage.AddMember("content", "You are an AI language model trained to answer questions and engage in conversation.", lrcAllocator);
  lcMessages.PushBack(lcSystemMessage, lrcAllocator);

  rapidjson::Value lcUserMessage(rapidjson::kObjectType);
  lcUserMessage.AddMember("role", "user", lrcAllocator);
  lcUserMessage.AddMember("content", rapidjson::Value().SetString(arcPrompt.c_str(), arcPrompt.length(), lrcAllocator), lrcAllocator);
  lcMessages.PushBack(lcUserMessage, lrcAllocator);

  lcRequestBody.AddMember("messages", lcMessages, lrcAllocator);
  lcRequestBody.AddMember("max_tokens", 100, lrcAllocator);

  rapidjson::StringBuffer lcBuffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(lcBuffer);
  lcRequestBody.Accept(writer);

  return lcBuffer.GetString();
}

std::string tcGptCommand::MakeGptApiCall(const std::string& arcRequestBody)
{
  std::stringstream lcResponse;

  try
  {
    curlpp::Cleanup lcCleaner;
    curlpp::Easy lcRequest;

    std::list<std::string> headers;
    headers.push_back("Content-Type: application/json");
    headers.push_back("Authorization: Bearer " + mcApiKey);

    lcRequest.setOpt(new curlpp::options::HttpHeader(headers));
    lcRequest.setOpt(new curlpp::options::Url("https://api.openai.com/v1/chat/completions"));
    lcRequest.setOpt(new curlpp::options::PostFields(arcRequestBody));
    lcRequest.setOpt(new curlpp::options::PostFieldSize(arcRequestBody.length()));

    std::stringstream lcResponseStream;
    lcRequest.setOpt(new curlpp::options::WriteStream(&lcResponseStream));

    lcRequest.perform();

    std::string responseStr = lcResponseStream.str();
    rapidjson::Document lcJsonResponse;
    lcJsonResponse.Parse(responseStr.c_str());

    if (lcJsonResponse.HasParseError())
    {
      return "[Parsing error in JSON response]";
    }

    if (lcJsonResponse.HasMember("choices") && lcJsonResponse["choices"].IsArray() && lcJsonResponse["choices"].Size() > 0)
    {
      const auto& choice = lcJsonResponse["choices"][0];
      if (choice.HasMember("message") && choice["message"].IsObject() && choice["message"].HasMember("content") && choice["message"]["content"].IsString())
      {
        return choice["message"]["content"].GetString();
      }
      else
      {
        return "[Received unexpected JSON response structure]";
      }
    }
    else
    {
      return "[Empty JSON received from API call]";
    }
  }
  catch (const curlpp::RuntimeError& lrxException)
  {
    return "[Encountered runtime error=" + std::string(lrxException.what()) + "]";
  }
  catch (const curlpp::LogicError& lrxException)
  {
    return "[Encountered logic error=" + std::string(lrxException.what()) + "]";
  }
  catch (const std::exception& lrxException)
  {
    return "[Caught exception=" + std::string(lrxException.what()) + "]";
  }
}
