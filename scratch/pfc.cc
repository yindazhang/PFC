#include "flow-schedule.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("PFC");

int
main(int argc, char* argv[])
{
	srand(11);
	
	flowFile = "test";

	double duration = 1.0;
	double startTime = 2.0;

	CommandLine cmd(__FILE__);
	cmd.AddValue("time", "the total run time (s), by default 1.0", duration);
	cmd.AddValue("startTime", "the start time (s), by default 2.0", startTime);
	cmd.AddValue("flow", "the flow file", flowFile);

    cmd.AddValue("cc", "the version of congestion control. 0 : no congestion control", ccVersion);
    cmd.AddValue("pfc", "the version of PFC. 0 : no PFC", pfcVersion);
    cmd.Parse(argc, argv);

    logFile = "logs/" + flowFile + "s_PFC" + std::to_string(pfcVersion) + "_CC" + std::to_string(ccVersion);
	BuildFatTree(logFile);
	std::cout << "Build Topology" << std::endl;

	ScheduleFlow();

	std::cout << "Start Application" << std::endl;
	auto start = std::chrono::system_clock::now();

	Simulator::Stop(Seconds(startTime + duration + 5));
	Simulator::Run();
	Simulator::Destroy();

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> diff = end - start;
	std::cout << "Used time: " << diff.count() << "s." << std::endl;
}