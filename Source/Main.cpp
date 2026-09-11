#include "stdafx.h"

#include "ServerApplication.h"

int main(int argC, char** argV)
{
	try
	{
		ServerApplication app(argC, argV);

		const auto error = app.RunApplication();
		if (error)
		{
			LogError(error);
		}

		return error.GetErrorCodeInt();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Unhandled server error: " << exception.what() << '\n';
		return static_cast<int>(Felis::EApplicationErrorCode::RuntimeFailed);
	}
}
