#ifndef _MESSAGEPROCESSOR_H
#define _MESSAGEPROCESSOR_H

class MessageProcessor

{
private:
	bool cont;
	void migrationMessage(long key, LTFSDmCommServer *command);
	void selRecallMessage(long key, LTFSDmCommServer *command);
	void requestNumber(long key, LTFSDmCommServer *command);
	void stop(long key, LTFSDmCommServer *command);
	void status(long key, LTFSDmCommServer *command);
public:
	MessageProcessor() : cont(true) {}
	~MessageProcessor() {};
	void run(std::string label, long key, LTFSDmCommServer command);
};

#endif /* _MESSAGEPROCESSOR_H */
