
//OpenSCADA module Protocol.SelfSystem file: self.cpp
/***************************************************************************
 *   Copyright (C) 2007-2016 by Roman Savochenko, <rom_as@oscada.org>      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; version 2 of the License.               *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <string.h>
#include <string>

#include <tsys.h>
#include <tmess.h>
#include <tmodule.h>
#include "self.h"

//*************************************************
//* Modul info!                                   *
#define MOD_ID		"SelfSystem"
#define MOD_NAME	_("Self protocol of OpenSCADA")
#define MOD_TYPE	SPRT_ID
#define VER_TYPE	SPRT_VER
#define MOD_VER		"1.2.2"
#define AUTHORS		_("Roman Savochenko")
#define DESCRIPTION	_("Provides self OpenSCADA protocol based at XML and one's control interface.")
#define LICENSE		"GPL2"
//*************************************************

SelfPr::TProt *SelfPr::mod;

extern "C"
{
#ifdef MOD_INCL
    TModule::SAt prot_SelfSystem_module( int n_mod )
#else
    TModule::SAt module( int n_mod )
#endif
    {
	if(n_mod == 0)	return TModule::SAt(MOD_ID,MOD_TYPE,VER_TYPE);
	return TModule::SAt("");
    }

#ifdef MOD_INCL
    TModule *prot_SelfSystem_attach( const TModule::SAt &AtMod, const string &source )
#else
    TModule *attach( const TModule::SAt &AtMod, const string &source )
#endif
    {
	if(AtMod == TModule::SAt(MOD_ID,MOD_TYPE,VER_TYPE)) return new SelfPr::TProt(source);
	return NULL;
    }
}

using namespace SelfPr;

//*************************************************
//* TProt                                         *
//*************************************************
TProt::TProt( string name ) : TProtocol(MOD_ID), mTAuth(60), mComprLev(0), mComprBrd(80), mSingleUserHostLimit(10)
{
    mod = this;

    modInfoMainSet(MOD_NAME, MOD_TYPE, MOD_VER, AUTHORS, DESCRIPTION, LICENSE, name);
}

TProt::~TProt( )
{

}

int TProt::sesOpen( const string &user, const string &pass, const string &src )
{
    string pHash;
    if(!SYS->security().at().usrPresent(user) || !SYS->security().at().usrAt(user).at().auth(pass,&pHash)) return -1;

    MtxAlloc res(dataRes(), true);

    //Check sesions for close old and reuse more other
    unsigned i_oCnt = 0;
    map<int, SAuth>::iterator aOldI = mAuth.end();
    for(map<int, SAuth>::iterator aI = mAuth.begin(); aI != mAuth.end(); )
	if(time(NULL) > (aI->second.tAuth+authTime()*60)) mAuth.erase(aI++);	//Long unused
	else {
	    if(aI->second.name == user && aI->second.src == src) {	//More opened
		if(aOldI == mAuth.end() || aI->second.tAuth < aOldI->second.tAuth) aOldI = aI;
		++i_oCnt;
	    }
	    ++aI;
	}
    if((int)i_oCnt > singleUserHostLimit() && aOldI != mAuth.end()) {
	mess_err(nodePath().c_str(), _("Connections from the user '%s' and the source '%s' reached to limit %d. Erasing spare!"),
	    user.c_str(), TSYS::strLine(src,0).c_str(), singleUserHostLimit());
	mAuth.erase(aOldI);
    }

    //New session ID generation
    int idSes = rand();
    while(mAuth.find(idSes) != mAuth.end()) idSes = rand();

    //Make new sesion
    mAuth[idSes] = TProt::SAuth(time(NULL), user, src);
    mAuth[idSes].pHash = pHash;

    return idSes;
}

void TProt::sesClose( int idSes )
{
    MtxAlloc res(dataRes(), true);
    mAuth.erase(idSes);
}

TProt::SAuth TProt::sesGet( int idSes )
{
    MtxAlloc res(dataRes(), true);
    map<int, SAuth>::iterator aI = mAuth.find(idSes);
    if(aI != mAuth.end()) {
	time_t cur_tm = time(NULL);
	if(cur_tm > (aI->second.tAuth+authTime()*60)) mAuth.erase(aI);
	else {
	    aI->second.tAuth = cur_tm;
	    return aI->second;
	}
    }

    return TProt::SAuth();
}

void TProt::sesSet( int idSes, const SAuth &auth )
{
    MtxAlloc res(dataRes(), true);
    mAuth[idSes] = auth;
}

void TProt::load_( )
{
    //Load parameters from command line

    //Load parameters from config-file
    setAuthTime(s2i(TBDS::genDBGet(nodePath()+"SessTimeLife",i2s(authTime()))));
    setComprLev(s2i(TBDS::genDBGet(nodePath()+"ComprLev",i2s(comprLev()))));
    setComprBrd(s2i(TBDS::genDBGet(nodePath()+"ComprBrd",i2s(comprBrd()))));
    setSingleUserHostLimit(s2i(TBDS::genDBGet(nodePath()+"SingleUserHostLimit",i2s(singleUserHostLimit()))));
}

void TProt::save_( )
{
    TBDS::genDBSet(nodePath()+"SessTimeLife",i2s(authTime()));
    TBDS::genDBSet(nodePath()+"ComprLev",i2s(comprLev()));
    TBDS::genDBSet(nodePath()+"ComprBrd",i2s(comprBrd()));
    TBDS::genDBSet(nodePath()+"SingleUserHostLimit",i2s(singleUserHostLimit()));
}

TProtocolIn *TProt::in_open( const string &name )	{ return new TProtIn(name); }

void TProt::outMess( XMLNode &io, TTransportOut &tro )
{
    char buf[1000];
    string req, resp, header;
    int rez, resp_len, off, head_end;

    MtxAlloc res(tro.reqRes(), true);

    bool isDir = s2i(io.attr("rqDir"));	io.attrDel("rqDir");
    bool authForce = s2i(io.attr("rqAuthForce")); io.attrDel("rqAuthForce");
    int conTm = s2i(io.attr("conTm"));	io.attrDel("conTm");
    string user = io.attr("rqUser");	io.attrDel("rqUser");
    string pass = io.attr("rqPass");	io.attrDel("rqPass");
    string data = io.save();
    io.clear();

    try {
	while(true) {
	    //Session open
	    if(!isDir && (tro.prm1() < 0 || authForce)) {
		req = "SES_OPEN " + user + " " + pass + "\x0A";

		if(mess_lev() == TMess::Debug) mess_debug(nodePath().c_str(), _("SES_OPEN request: %d"), req.size());

		resp_len = tro.messIO(req.c_str(), req.size(), buf, sizeof(buf)-1, conTm);
		resp.assign(buf, resp_len);

		// Wait tail
		while((header=TSYS::strLine(resp.c_str(),0)).size() >= resp.size() || resp[header.size()] != '\x0A') {
		    if(!(resp_len=tro.messIO(NULL,0,buf,sizeof(buf)))) throw TError(nodePath().c_str(),_("Not full respond."));
		    resp.append(buf, resp_len);
		}

		if(mess_lev() == TMess::Debug) mess_debug(nodePath().c_str(), _("SES_OPEN response: %d"), resp_len);

		off = 0;
		if(header.size() >= 5 && TSYS::strParse(header,0," ",&off) == "REZ") {
		    rez = s2i(TSYS::strParse(header,0," ",&off));
		    if(rez > 0 || off >= (int)header.size()) {
			if(rez == atoi(ERR_AUTH)) { io.setAttr("rez",i2s(rez))->setText(header.substr(off)); break; }
			throw TError(nodePath().c_str(), _("Station '%s' error: %s!"), tro.id().c_str(), header.substr(off).c_str());
		    }
		    tro.setPrm1(s2i(header.substr(off)));
		} else throw TError(nodePath().c_str(), _("Station '%s' error: Respond format error!"), tro.id().c_str());
	    }

	    //Request
	    // Compress data
	    bool reqCompr = (comprLev() && (int)data.size() > comprBrd());
	    if(reqCompr) data = TSYS::strCompr(data, comprLev());

	    if(isDir)	req = "REQDIR " + user + " " + pass + " " + i2s(data.size()*(reqCompr?-1:1)) + "\x0A" + data;
	    else	req = "REQ " + i2s(tro.prm1()) + " " + i2s(data.size()*(reqCompr?-1:1)) + "\x0A" + data;

	    if(mess_lev() == TMess::Debug) mess_debug(nodePath().c_str(), _("REQ send: %d"), req.size());

	    resp_len = tro.messIO(req.c_str(), req.size(), buf, sizeof(buf), conTm);
	    resp.assign(buf, resp_len);

	    // Wait tail
	    while((header=TSYS::strLine(resp.c_str(),0)).size() >= resp.size() || resp[header.size()] != '\x0A') {
		if(!(resp_len=tro.messIO(NULL,0,buf,sizeof(buf)))) throw TError(nodePath().c_str(),_("Not full respond."));
		resp.append(buf, resp_len);
	    }

	    if(mess_lev() == TMess::Debug) mess_debug(nodePath().c_str(), _("REQ response first %d"), resp.size());

	    // Get head
	    head_end = header.size() + 1;
	    off = 0;
	    //header = TSYS::strLine(resp,0,&head_end);
	    if(header.size() < 5 || TSYS::strParse(header,0," ",&off) != "REZ")
		throw TError(nodePath().c_str(),_("Station respond '%s' error!"),tro.id().c_str());
	    rez = s2i(TSYS::strParse(header,0," ",&off));
	    if(rez == atoi(ERR_AUTH)) {
		tro.setPrm1(-1);
		if(isDir) { io.setAttr("rez",i2s(rez))->setText(header.substr(off)); break; }
		else continue;
	    }
	    if(rez > 0 || off >= (int)header.size())
		throw TError(nodePath().c_str(),_("Station '%s' error: %s!"),tro.id().c_str(),buf+off);
	    int resp_size = s2i(header.substr(off));

	    // Wait tail
	    while((int)resp.size() < abs(resp_size)+head_end) {
		resp_len = tro.messIO(NULL, 0, buf, sizeof(buf));
		if(!resp_len) throw TError(nodePath().c_str(),_("Not full respond."));
		resp.append(buf, resp_len);
	    }

	    if(mess_lev() == TMess::Debug) mess_debug(nodePath().c_str(), _("REQ response full %d"), resp.size());

	    if(resp_size < 0) io.load(TSYS::strUncompr(resp.substr(head_end)), XMLNode::LD_NoTxtSpcRemEnBeg);
	    else io.load(resp.substr(head_end), XMLNode::LD_NoTxtSpcRemEnBeg);

	    return;
	}
    } catch(TError &err) {
	if(mess_lev() == TMess::Debug) mess_debug(nodePath().c_str(), _("Request error: %s"), err.mess.c_str());
	tro.stop();
	throw;
    }
}

void TProt::cntrCmdProc( XMLNode *opt )
{
    //Get page info
    if(opt->name() == "info") {
	TProtocol::cntrCmdProc(opt);
	if(ctrMkNode("area",opt,1,"/prm",_("Parameters"))) {
	    if(ctrMkNode("area",opt,1,"/prm/st",_("State")))
		ctrMkNode("list",opt,-1,"/prm/st/auths",_("Active authentication sessions"),R_R_R_,"root",SPRT_ID);
	    if(ctrMkNode("area",opt,1,"/prm/cfg",_("Module options"))) {
		ctrMkNode("fld",opt,-1,"/prm/cfg/lf_tm",_("Life time of auth session(min)"),RWRWR_,"root",SPRT_ID,2,"tp","dec","min","1");
		ctrMkNode("fld",opt,-1,"/prm/cfg/sUserHostLim",_("Single user and host connections limit"),RWRWR_,"root",SPRT_ID,1,"tp","dec");
		ctrMkNode("fld",opt,-1,"/prm/cfg/compr",_("Compression level"),RWRWR_,"root",SPRT_ID,4,"tp","dec","min","-1","max","9",
		    "help",_("ZLib compression level:\n"
			     "  -1  - optimal speed-size;\n"
			     "  0   - disable;\n"
			     "  1-9 - direct level."));
		ctrMkNode("fld",opt,-1,"/prm/cfg/comprBrd",_("Lower compression border"),RWRWR_,"root",SPRT_ID,3,"tp","dec","min","10",
		    "help",_("Value in bytes."));
	    }
	}
	return;
    }

    //Process command to page
    string a_path = opt->attr("path");
    if(a_path == "/prm/st/auths" && ctrChkNode(opt)) {
	MtxAlloc res(dataRes(), true);
	for(map<int,SAuth>::iterator authEl = mAuth.begin(); authEl != mAuth.end(); ++authEl)
	    opt->childAdd("el")->setText(TSYS::strMess(_("%s %s(%s)"),
		atm2s(authEl->second.tAuth).c_str(),authEl->second.name.c_str(),authEl->second.src.c_str()));
    }
    else if(a_path == "/prm/cfg/lf_tm") {
	if(ctrChkNode(opt,"get",RWRWR_,"root",SPRT_ID,SEC_RD))	opt->setText(i2s(authTime()));
	if(ctrChkNode(opt,"set",RWRWR_,"root",SPRT_ID,SEC_WR))	setAuthTime(s2i(opt->text()));
    }
    else if(a_path == "/prm/cfg/sUserHostLim") {
	if(ctrChkNode(opt,"get",RWRWR_,"root",SPRT_ID,SEC_RD))	opt->setText(i2s(singleUserHostLimit()));
	if(ctrChkNode(opt,"set",RWRWR_,"root",SPRT_ID,SEC_WR))	setSingleUserHostLimit(s2i(opt->text()));
    }
    else if(a_path == "/prm/cfg/compr") {
	if(ctrChkNode(opt,"get",RWRWR_,"root",SPRT_ID,SEC_RD))	opt->setText(i2s(comprLev()));
	if(ctrChkNode(opt,"set",RWRWR_,"root",SPRT_ID,SEC_WR))	setComprLev(s2i(opt->text()));
    }
    else if(a_path == "/prm/cfg/comprBrd") {
	if(ctrChkNode(opt,"get",RWRWR_,"root",SPRT_ID,SEC_RD))	opt->setText(i2s(comprBrd()));
	if(ctrChkNode(opt,"set",RWRWR_,"root",SPRT_ID,SEC_WR))	setComprBrd(s2i(opt->text()));
    }
    else TProtocol::cntrCmdProc(opt);
}

//*************************************************
//* TProtIn                                       *
//*************************************************
TProtIn::TProtIn( string name ) : TProtocolIn(name)
{

}

TProtIn::~TProtIn( )
{

}

bool TProtIn::mess( const string &request, string &answer )
{
    int64_t d_tm = 0;
    int ses_id = -1;
    int req_sz = 0;
    char user[256] = "", pass[256] = "";

    reqBuf += request;

    if(mess_lev() == TMess::Debug) d_tm = TSYS::curTime();

    //Check for completeness header
    string req = TSYS::strLine(reqBuf, 0);
    if(req.size() >= reqBuf.size() || reqBuf[req.size()] != '\x0A') return true;

    if(req.compare(0,8,"SES_OPEN") == 0) {
	sscanf(req.c_str(), "SES_OPEN %255s %255s", user, pass);
	if((ses_id=mod->sesOpen(user,pass,TSYS::strLine(srcAddr(),0))) < 0)
	    answer = "REZ " ERR_AUTH " Auth error. User or password error.\x0A";
	else answer = "REZ " ERR_NO " " + i2s(ses_id) + "\x0A";
    }
    else if(req.compare(0,9,"SES_CLOSE") == 0) {
	sscanf(req.c_str(), "SES_CLOSE %d", &ses_id);
	mod->sesClose(ses_id);
	answer = "REZ 0\x0A";
    }
    else if(req.compare(0,3,"REQ") == 0) {
	if(mess_lev() == TMess::Debug) {
	    mess_debug(nodePath().c_str(), _("Get request: '%s': %d, time: %f ms."),
		req.c_str(), reqBuf.size(), 1e-3*(TSYS::curTime()-d_tm));
	    d_tm = TSYS::curTime();
	}

	TProt::SAuth auth;
	if(sscanf(req.c_str(),"REQ %d %d",&ses_id,&req_sz) == 2) auth = mod->sesGet(ses_id);
	else if(sscanf(req.c_str(),"REQDIR %255s %255s %d",user,pass,&req_sz) == 3) {
	    if(SYS->security().at().usrPresent(user) && SYS->security().at().usrAt(user).at().auth(pass,&auth.pHash))
	    { auth.tAuth = 1; auth.name = user; }
	}
	else { answer = "REZ " ERR_CMD " Command format error.\x0A"; reqBuf.clear(); return false; }

	if(!auth.tAuth) { answer = "REZ " ERR_AUTH " Auth error. Session no valid.\x0A"; reqBuf.clear(); return false; }

	try {
	    if(reqBuf.size() < (req.size()+1+abs(req_sz))) return true;

	    //Decompress request
	    if(req_sz < 0) reqBuf.replace(req.size()+1, abs(req_sz), TSYS::strUncompr(reqBuf.substr(req.size()+1)));

	    //Process request
	    XMLNode req_node;
	    req_node.load(reqBuf.substr(req.size()+1), XMLNode::LD_NoTxtSpcRemEnBeg);

	    // Check for reforward requests
	    string host = req_node.attr("reforwardHost");
	    if(host.size()) {
		if(mess_lev() == TMess::Debug) {
		    mess_debug(nodePath().c_str(), _("Check and reforward request: '%s': %d, time: %f ms."),
			req.c_str(), reqBuf.size(), 1e-3*(TSYS::curTime()-d_tm));
		    d_tm = TSYS::curTime();
		}
		req_node.setAttr("path", "/"+host+req_node.attr("path"))->setAttr("reforwardHost", "");
		try { SYS->transport().at().cntrIfCmd(req_node, "Reforward", auth.name); }
		catch(TError &err) {
		    req_node.childClear();
		    req_node.setAttr("mcat",err.cat)->setAttr("rez","10")->setText(err.mess);
		}
	    }
	    else {
		req_node.setAttr("user", auth.name);

		if(mess_lev() == TMess::Debug) {
		    mess_debug(nodePath().c_str(), _("Unpack and load request: '%s': %d, time: %f ms."),
			req.c_str(), reqBuf.size(), 1e-3*(TSYS::curTime()-d_tm));
		    d_tm = TSYS::curTime();
		}

		req_node.setAttr("remoteSrcAddr", TSYS::strLine(srcAddr(),0));
		SYS->cntrCmd(&req_node);
		if(auth.pHash.size()) {
		    req_node.setAttr("pHash", auth.pHash);
		    if(ses_id >= 0) { auth.pHash = ""; mod->sesSet(ses_id, auth); }
		}

		if(mess_lev() == TMess::Debug) {
		    mess_debug(nodePath().c_str(), _("Process request: '%s', time: %f ms."), req.c_str(), 1e-3*(TSYS::curTime()-d_tm));
		    d_tm = TSYS::curTime();
		}
	    }

	    string resp = req_node.save(XMLNode::MissTagEnc|XMLNode::MissAttrEnc)+"\n";

	    //Compress respond
	    bool respCompr = (((TProt&)owner()).comprLev() && (int)resp.size() > ((TProt&)owner()).comprBrd());
	    if(respCompr) resp = TSYS::strCompr(resp,((TProt&)owner()).comprLev());

	    if(mess_lev() == TMess::Debug)
		mess_debug(nodePath().c_str(), _("Save respond to stream and pack: '%s': %d, time: %f ms."),
		    req.c_str(), resp.size(), 1e-3*(TSYS::curTime()-d_tm));

	    answer = "REZ " ERR_NO " " + i2s(resp.size()*(respCompr?-1:1)) + "\x0A" + resp;
	} catch(TError &err) { answer = "REZ " ERR_PRC " " + err.cat + ":" + err.mess + "\x0A"; }
    }
    else answer = "REZ " ERR_CMD " Command format error.\x0A";

    reqBuf.clear();

    return false;
}