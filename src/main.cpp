#include <dpp/dpp.h>
#include <ctime>
#include <string>
#include <sstream>

const std::string read_string_from_file(const std::string &file_path)
{
    const std::ifstream input_stream(file_path, std::ios_base::binary);

    if (input_stream.fail()) 
    {
        throw std::runtime_error("Failed to open token file");
    }

    std::stringstream buffer;
    buffer << input_stream.rdbuf();
    std::string ret_string=buffer.str();
    ret_string.erase(std::remove_if(ret_string.begin(),
                                    ret_string.end(),
                                    [](auto ch)
                                    {
                                        return (ch == '\n' ||
                                                ch == '\r');
                                    }),
                     ret_string.end());
    return ret_string;
}

int main()
{
    // get bot token
    std::string botToken=read_string_from_file("/home/sigmar/.secrets/bot.token");

    // instantiate bot
	dpp::cluster bot(botToken);

	bot.on_log(dpp::utility::cout_logger());

	bot.on_slashcommand([](const dpp::slashcommand_t& event) 
	{
		if (event.command.get_command_name() == "help")
		{
			event.reply("Right now you can only '/date' me :guraW:");
		} 

        else if (event.command.get_command_name() == "date") 
        {
            time_t now = time(0);
            char* dt = ctime(&now);
            std::ostringstream oss; 
            oss << "It is now " << dt << " :xqcL";
            std::string response = oss.str();
            event.reply(response);
        }
	});

	// register for help command
	bot.on_ready([&bot](const dpp::ready_t& event) 
	{
		if (dpp::run_once<struct register_bot_commands>()) 
		{
			bot.global_command_create(dpp::slashcommand("help", "Provide usage", bot.me.id));
            bot.global_command_create(dpp::slashcommand("date", "Go on a date", bot.me.id));
        }
	});

	bot.start(false);
}

