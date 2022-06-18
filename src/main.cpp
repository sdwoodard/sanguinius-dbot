#include <dpp/dpp.h>
#include <ctime>
#include <string>
#include <sstream>
#include <vector>

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
		dpp::message response_message;
		if (event.command.get_command_name() == "help")
		{
			response_message.content="Help yourself, human.";
		}

        else if (event.command.get_command_name() == "date")
        {
            time_t now = time(0);
            char* dt = ctime(&now);
            std::ostringstream oss;
            oss << "It is now " << dt;
            response_message.content = oss.str();

        }
        else if (event.command.get_command_name() == "vote")
        {
        	std::string vote_response = std::get<std::string>(event.get_parameter("user_vote"));
        	if (vote_response == "Lucas")
        	{
        		response_message.content = "Registered vote for Mr. New Car.";
        	}
        	else if (vote_response == "Nicholas")
        	{
        		response_message.content = "Posting " + vote_response + "'s private information now.";
        	}
        	else if (vote_response == "Stephen")
        	{
        		response_message.content = vote_response + " is immune from this dark vote.";
        	}
        	else
        	{
        		response_message.content = "ERROR: Please only vote for Lucas or Nicholas.";
        	}

        }
        else if (event.command.get_command_name() == "repo")
        {
        	dpp::embed response_embed = dpp::embed().
        		set_color(dpp::colors::sti_blue).
				set_title("Source code for Sanguinius!").
				set_url("https://github.com/sdwoodard/sanguinius-dbot").
				set_author("Github", "https://github.com/", "https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png").
				set_description("Code repository for all things required to build the sanguinius discord bot.").
				set_thumbnail("https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png").
				add_field("Current regular supports:", "Stephen Woodard").
				set_image("https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png").
				set_footer(dpp::embed_footer().set_text("Come by sometime and coding yourself!")).
				set_timestamp(time(0));
        	response_message.add_embed(response_embed);
        }
		event.reply(response_message);
	});

	// register for help command
	bot.on_ready([&bot](const dpp::ready_t& event)
	{
		if (dpp::run_once<struct register_bot_commands>())
		{
			dpp::slashcommand votecommand("vote", "Vote for your desired user", bot.me.id);
			votecommand.add_option(
				dpp::command_option(dpp::co_string, "vote_response", "The user's real name", true).
					add_choice(dpp::command_option_choice("Lucas", std::string("user_Lucas"))).
					add_choice(dpp::command_option_choice("Nicholas", std::string("user Nicholas"))).
					add_choice(dpp::command_option_choice("Stephen", std::string("user Stephen")
					)
				)
			);

			bot.global_command_create(dpp::slashcommand("help", "Provide usage", bot.me.id));
            bot.global_command_create(dpp::slashcommand("date", "Go on a date", bot.me.id));
            bot.global_command_create(votecommand);
            bot.global_command_create(dpp::slashcommand("repo", "Start programming!", bot.me.id));
        }
	});

	bot.start(false);
}
