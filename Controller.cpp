/*
 * ActionReceiverMediator.cpp
 *
 *  Created on: 17.08.2016
 *      Author: sven
 */

#include <Arduino.h>
#include <FS.h>
#include "Controller.h"
#include "Utils.h"
#include "Logger.h"
#include "Consts.h"


Controller::Controller() {
	if (!SPIFFS.begin()) {
		Logger::getInstance()->addToLog("SPIFFS konnte nicht genutzt werden!");
	}
	EMERGENCYActive = false;
}

Controller::~Controller() {
}

void Controller::registerCmdReceiver(CmdReceiverBase* base) {
	receiver.add(base);
	loops.add(base);
}

void Controller::doLoops() {
	int idx;
	// Make sure that the nextRun List is as long as the loop list
	while (nextRun.size() < loops.size()) {
		nextRun.add(0);
	}
	// Run and save the new execute time
	unsigned long now = millis();
	for (idx = 0; idx < loops.size(); idx++) {
		if (nextRun.get(idx)  <= now) {
			int wait = loops.get(idx)->loop();
			nextRun.set(idx, millis() + wait);
		}
		delay(0);
	}
	delay(0);
	if (dnsServer) {
		dnsServer->processNextRequest();
	}
}

void Controller::registerNotify(INotify* base) {
	if (base == NULL) {
		Logger::getInstance()->addToLog("Null in registeryNotify");
		return;
	}
	actions.add(base);
}

void Controller::registerLoop(ILoop* loop) {
	if (loop == NULL) {
		Logger::getInstance()->addToLog("Null in registeryLoop");
		return;
	}
	loops.add(loop);
}

void Controller::notifyTurnout(int id, int direction, int source) {
	// ignore the same command within 50 msec
	if (lastTurnoutCmd[0] == id && lastTurnoutCmd[1] == direction
			&& (millis() - lastTurnoutCmd[2]) < 50) {
		lastTurnoutCmd[2] = millis();
		return;
	}
	TurnOutData* data = getTurnOutData(id);
	data->direction = direction;

	Logger::getInstance()->addToLog(
			"Turnout-CMD [ID:" + String(id) + "/ D:" + String(direction) + "]");
	lastTurnoutCmd[0] = id;
	lastTurnoutCmd[1] = direction;
	lastTurnoutCmd[2] = millis();

	// Send the information to the actions
	int idx;
	for (idx = 0; idx < actions.size(); idx++) {
		actions.get(idx)->TurnoutCmd(id, direction, source);
	}
}

String Controller::getHTMLController() {
	String msg = "<div class=\"container\">";
	for (int idx = 0; idx < settings.size(); idx++) {
		msg += settings.get(idx)->getHTMLController(
				"/set?id=" + String(idx) + "&");
		msg += "\n";
	}
	msg += "</div>";
	return msg;
}

String Controller::getHTMLCfg() {
	String msg = "<div class=\"container\">";
	for (int idx = 0; idx < settings.size(); idx++) {
		msg += settings.get(idx)->getHTMLCfg(
				"/set?id=" + String(idx) + "&");
		msg += "\n";
	}
	msg += "</div>";
	return msg;
}

void Controller::setRequest(String id, String key, String value) {
	int idx = id.toInt();
	Serial.println(idx);
	if (idx >= settings.size()) {
		return;
	}
	settings.get(idx)->setSettings(key, value);
}

LocData* Controller::getLocData(int id) {
	LocData* data;
	if (items.find(id) == items.end()) {
		data = new LocData();
		data->status = 0;
		data->speed = 0;
		data->speedsteps = 0;
		if (id != Consts::LOCID_ALL) {
			items[id] = data;
		}
		return data;
	}
	return items[id];
}

TurnOutData* Controller::getTurnOutData(int id) {
	TurnOutData* data;
	if (turnoutinfo.find(id) == turnoutinfo.end()) {
		data = new TurnOutData();
		data->direction = 0;
		turnoutinfo[id] = data;
		return data;
	}
	return turnoutinfo[id];
}

/**
 * @param speed see Consts.h
 * @param direction 1 = forward / 1 = reverse
 */
void Controller::notifyDCCSpeed(int id, int speed, int direction,
		int SpeedSteps, int source) {
	if (direction == 0) {
		Logger::getInstance()->addToLog("Ungültige Richtung (0)");
	}
	EMERGENCYActive = speed == Consts::SPEED_EMERGENCY;
	// Filter out known commands
	LocData* data;
	if (items.find(id) == items.end()) {
		data = new LocData();
		if (id != Consts::LOCID_ALL) {
			items[id] = data;
		}
	} else {
		data = items[id];
		if (data->direction == direction && data->speed == speed
				&& data->speedsteps == SpeedSteps) {
			return;
		}
	}

	// Save new state
	data->direction = direction;
	data->speed = speed;
	data->speedsteps = SpeedSteps;
	Serial.println(
			"DCC-Speed: " + String(id) + " D: " + String(direction) + " "
					+ String(speed) + " " + String(SpeedSteps));

	// Send the information to the actions
	int idx;
	for (idx = 0; idx < actions.size(); idx++) {
		actions.get(idx)->DCCSpeed(id, speed, direction, SpeedSteps, source);
	}
	if (id == Consts::LOCID_ALL) {
		delete data;
	}
}


void Controller::notifyDCCFun(int id, int bit, unsigned int newBitValue, int source) {
	// Get the old status ...
	LocData* data = getLocData(id);
	boolean changed = false;
	// .. and send only the changed bits
	unsigned long int value = data->status;
	unsigned long int oldBitValue = bit_is_set(value, bit);
	if (oldBitValue == newBitValue) {
		return;
	}
	changed = true;
	if (newBitValue == 0) {
		clear_bit(data->status, bit);
	} else {
		set_bit(data->status, bit);
	}
	for (int idx = 0; idx < actions.size(); idx++) {
		actions.get(idx)->DCCFunc(id, bit, (newBitValue == 0) ? 0 : 1, source);
	}
	if (changed) {
		Serial.println("Func " + String(id) + " " + String(data->status));
		for (int idx = 0; idx < actions.size(); idx++) {
			actions.get(idx)->DCCFunc(id, data->status, source);
		}
	}
}


void Controller::notifyDCCFun(int id, int startbit, int stopbit, unsigned long partValues, int source) {
	// Get the old status ...
	LocData* data = getLocData(id);

	boolean changed = false;
	// .. and send only the changed bits
	unsigned long int value = data->status;
	for (int i = startbit; i <= stopbit; i++) {
		unsigned long int oldBitValue = bit_is_set(value, i);
		unsigned long int newBitValue = bit_is_set(partValues, i);
		if (oldBitValue == newBitValue) {
			continue;
		}
		changed = true;
		if (newBitValue == 0) {
			clear_bit(data->status, i);
		} else {
			set_bit(data->status, i);
		}
		for (int idx = 0; idx < actions.size(); idx++) {
			actions.get(idx)->DCCFunc(id, i, (newBitValue == 0) ? 0 : 1, source);
		}
 	}
	if (changed) {
		Serial.println("Func " + String(id) + " " + String(data->status));
		for (int idx = 0; idx < actions.size(); idx++) {
			actions.get(idx)->DCCFunc(id, data->status, source);
		}
	}
}

String Controller::getHostname() {
	return "ly-dcc-" + Utils::getMAC();
}

void Controller::registerCmdSender(CmdSenderBase* base) {
	sender.add(base);
}

void Controller::updateRequestList() {
	requestList.clear();
	// Create Requestlist from all actions-Items
	for (int idx = 0; idx < actions.size(); idx++) {
		INotify* b = actions.get(idx);
		LinkedList<INotify::requestInfo*>*  list = b->getRequestList();
		for (int i = 0; i < list->size(); i++) {
			INotify::requestInfo* e = list->get(i);
			if (!requestListContains(e)) {
				requestList.add(e);
			}
		}
	}
	// Set Requestlist for all Senders
	for (int idx = 0; idx < sender.size(); idx++) {
		CmdSenderBase* s = sender.get(idx);
		s->setRequestList(&requestList);
	}

}

boolean Controller::requestListContains(INotify::requestInfo* element) {
	for (int idx = 0; idx < requestList.size(); idx++) {
		INotify::requestInfo* ri = requestList.get(idx);
		if ((ri->art == element->art) && (ri->id == element->id)) {
			return true;
		}
	}
	return false;
}
void Controller::emergencyStop(int source) {
	notifyDCCSpeed(Consts::LOCID_ALL, Consts::SPEED_EMERGENCY,
				   Consts::SPEED_FORWARD, 128, source);
}

void Controller::enableAPModus() {
	Logger::getInstance()->addToLog("Aktiviere Access Point!");
 	WiFiMode_t mode = WiFi.getMode();
	if (mode == WIFI_AP || mode == WIFI_AP_STA) {
		Logger::getInstance()->addToLog("Access Point bereits aktiv!");
		return;
	}
	int status = WiFi.status();
	if (status == WL_NO_SSID_AVAIL || status == WL_DISCONNECTED) {
		Serial.println("Station Modus abgeschalten");
		WiFi.disconnect();
	}
	WiFi.softAP("Hallo World");
	WiFi.enableAP(true);
	Serial.println("IP für AP: " + WiFi.softAPIP().toString());
	dnsServer.reset(new DNSServer());
	dnsServer->start(53, "*", WiFi.softAPIP());
}

bool Controller::isEmergency() {
	return EMERGENCYActive;
}

LinkedList<ISettings*>* Controller::getSettings() {
	return &settings;
}

void Controller::registerSettings(ISettings* loop) {
	if (loop == NULL) {
		Logger::getInstance()->addToLog("Null in registerySettings");
		return;
	}
	settings.add(loop);
}
