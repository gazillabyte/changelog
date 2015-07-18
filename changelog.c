/* 

changelog - A program for capturing and displaying changes and enhanvements.
Copyright (C) 2010-2015 Gerard J Nicol <gerard.nicol@gazillabyte.com>

This program is free software; you can redistribute it and/or
modify it as you see fit.

*/

#include <stdlib.h>
#include <getopt.h>
#include <stdio.h>
#include <db.h>
#include <memory.h>
#include <string.h>
#include <fnmatch.h>
#include <time.h>
#include <ESP/ESPReport.h>
#include <cgi.h>
#include <limits.h>

#define DBOK(rc) (rc==0)
#define DBNOTOK(rc) (rc!=0)

#define DBDBTINIT(f, s, a) { memset(&f, 0, sizeof(f)); \
	f.flags=DB_DBT_USERMEM; \
	f.size=f.ulen=s; \
	f.data=a;}

#ifndef WIN32
#define _strdup strdup
#define _MAX_PATH PATH_MAX	

#define FNM_FILE_NAME   FNM_PATHNAME   /* Preferred GNU name.  */
#define FNM_LEADING_DIR (1 << 3)       /* Ignore `/...' after a match.  */
#define FNM_CASEFOLD    (1 << 4)       /* Compare without regard to case.  */
#define FNM_EXTMATCH    (1 << 5)       /* Use ksh-like extended matching. */

#endif

struct DBCONTROL {
	DB_ENV						*pENV;
	DB							*pDBChanges;
	DB							*pDBModules;
	DB							*pDBNotes;
	int							bDBOK;
	unsigned int				nModules;
	char						*psModules[2048];
};

int DBOpen(char* psDBHome, struct DBCONTROL *pControl);

int ProcessModule(struct DBCONTROL *pControl, char* psBuffer);
int SetModuleScope(struct DBCONTROL *pControl, char* psBuffer);
int AddChange(struct DBCONTROL *pControl, char* psBuffer);
int ListChange(struct DBCONTROL *pControl, char* psSearch);
int ListChangeRSS(struct DBCONTROL *pControl, char* psModule);
int ListChangeDocbook(struct DBCONTROL *pControl, char* psSearch);

int main(int argc, char *argv[])
{
    int					c;
	char*				psHomeDir = ".";
	char*				psSearch = "";
	char*				psModule = "";
	struct DBCONTROL	oDB;
	int					nRC;
	char				sBuffer[2048];
	char*				p;
	unsigned int		i;
	char				*psREQURI = NULL;

	psREQURI = getenv("REQUEST_URI");

	if (psREQURI)
	{
		cgi_init();
		cgi_process_form();

		cgi_send_header("Content-type: application/rss+xml\n");

		psModule = cgi_param("Module");
		psHomeDir = "/var/db/changelog";
	}
	else
	{
		while ((c = getopt(argc, argv, "m:l:h:")) != -1)
		{
			switch (c)
			{
			case 'h':
				psHomeDir = optarg;
				break;
			case 'l':
				psSearch = optarg;
				break;
			case 'm':
				psModule = optarg;
				break;
			}
		}

		//*
		//* If we have a module argument then create a new file on stdout
		//*
		if (strlen(psModule))
		{
			fprintf(stdout, ":%s Module Description\n", psModule);
			fprintf(stdout, "@*(%s)\n", psModule);
			exit(EXIT_SUCCESS);
		}
	}

	//*
	//* Open the database stuff
	//*
	memset(&oDB, 0, sizeof(oDB));

	nRC = DBOpen(psHomeDir, &oDB); 
	
	if ( !nRC )
	{
		goto Exit_Cleanup;
	}

	if (psREQURI)
	{
		ListChangeRSS(&oDB, psModule);
	}
	else
	{
		if (strlen(psSearch))
		{
			if (getenv("DOCBOOK"))
			{
				ListChangeDocbook(&oDB, psSearch);
			}
			else
			{
				ListChange(&oDB, psSearch);
			}
		}
		else
		{
			while (fgets(sBuffer, sizeof(sBuffer), stdin))
			{
				p = sBuffer + strlen(sBuffer) - 1;
				if (*p == '\n')
				{
					*p = 0x00;
				}

				switch (sBuffer[0])
				{
				case ':':
					ProcessModule(&oDB, sBuffer);
					break;
				case '@':
					SetModuleScope(&oDB, sBuffer);
					break;
				case '!':
				case '+':
					AddChange(&oDB, sBuffer);
					break;
				}
			}
		}
	}

Exit_Cleanup:
	if ( oDB.pDBChanges )
	{
		oDB.pDBChanges->close(oDB.pDBChanges, 0);
	}

	if ( oDB.pDBModules )
	{
		oDB.pDBModules->close(oDB.pDBModules, 0);
	}

	if ( oDB.pDBNotes )
	{
		oDB.pDBNotes->close(oDB.pDBNotes, 0);
	}

	if ( oDB.bDBOK )
	{
		oDB.pENV->close(oDB.pENV, 0);
	}

	//*
	//* Free Module table
	//* 
	for(i=0; i < oDB.nModules; i++)
	{
		free(oDB.psModules[i]);
	}

	exit(EXIT_SUCCESS);
}

int DBFileOpen(DB_ENV *pENV, 
			   DB **pDB, 
			   char* psFile,
			   char* psDescription,
			   int nFlags)
{
	int nRC;
	DB	*pDB2;

	fprintf(stderr, "Opening Database: %s\n", psDescription);

	nRC = db_create(pDB, pENV, 0);
	
	if ( DBNOTOK(nRC) )
	{
		fprintf(stderr, "Failed to create Database Handle: %s\n", db_strerror(nRC));
		return 0;
	}
	
	pDB2=*pDB;
	
	nRC = pDB2->open(pDB2,
		NULL, 
		psFile, 
		NULL, 
		DB_BTREE, 
		nFlags | DB_AUTO_COMMIT | DB_CREATE,
		0664);
	
	fprintf(stderr, "Database Open Status: %s\n", db_strerror(nRC));

	return DBOK(nRC);
}

int DBOpen(char* psDBHome,
		   struct DBCONTROL *pControl)
{
	int		nRC;

	nRC = db_env_create(&pControl->pENV, 0);

	if ( nRC )
	{
		fprintf(stderr, "Failed to create Database Environment: %s\n", db_strerror(nRC));
		return 0;
	}
	
	pControl->pENV->set_errfile(pControl->pENV, stderr);
	pControl->pENV->set_flags(pControl->pENV, DB_CDB_ALLDB, 1);
	pControl->pENV->set_timeout(pControl->pENV, 1000, DB_SET_LOCK_TIMEOUT);  
	pControl->pENV->set_timeout(pControl->pENV, 10000, DB_SET_TXN_TIMEOUT); 
	
	nRC = pControl->pENV->open(pControl->pENV, 
		psDBHome, 
		DB_CREATE | DB_THREAD | DB_RECOVER | DB_INIT_LOG | DB_INIT_LOCK | DB_INIT_MPOOL | DB_INIT_TXN | DB_INIT_REP,
		0);
			
	fprintf(stderr, "Database Environment Open Status: %s\n", db_strerror(nRC));

	if ( DBNOTOK(nRC) )
	{
		pControl->pENV->close(pControl->pENV, 0);
		return 0;
	}

	nRC = DBFileOpen(pControl->pENV,
		&pControl->pDBChanges,
		"changelog.changes", 
	    "Changelog Change Table",
		0);

	if ( !nRC )
	{
		return 0;
	}

	nRC = DBFileOpen(pControl->pENV,
		&pControl->pDBModules,
		"changelog.modules", 
	    "Changelog Module Table",
		0);

	if ( !nRC )
	{
		return 0;
	}

	return 1;
}

int ProcessModule(struct DBCONTROL *pControl, char* psBuffer)
{
	char* psModule;
	char* psDescription;
	DBT	  key, data;
	int	  nRC;

	//*
	//* Skip past the control character
	//* 
	psBuffer++;

	psModule = strtok(psBuffer, " ");

	if ( !psModule || !strlen(psModule) )
	{
		return 0;
	}

	psDescription = strtok(NULL, ";");

	memset(&key, 0, sizeof(key));
	key.flags=DB_DBT_USERMEM;
	key.size=strlen(psModule);
	key.data=psModule;

	memset(&data, 0, sizeof(data));
	data.flags=DB_DBT_USERMEM;
	if ( psDescription )
	{
		data.size=strlen(psDescription);
		data.data=psDescription;
	}

	nRC = pControl->pDBModules->put(pControl->pDBModules, NULL, &key, &data, 0);

	return DBOK(nRC);
}

int SetModuleScope(struct DBCONTROL *pControl, char* psBuffer)
{
	DBT				key, data;
	int				nRC;
	unsigned int	i;
	DBC				*pDBC;
	char			sModule[_MAX_PATH + 1];

	//*
	//* Remove any previous module scopes
	//*
	for(i=0; i < pControl->nModules; i++)
	{
		free(pControl->psModules[i]);
		pControl->psModules[i]=NULL;
	}

	pControl->nModules=0;
	
	//*
	//* Skip past the control character
	//* 
	psBuffer++;

	nRC = pControl->pDBModules->cursor(pControl->pDBModules, NULL, &pDBC, 0);

	if ( DBNOTOK(nRC) )
	{
		return 0;
	}
		
	DBDBTINIT(key, sizeof(sModule)-1, &sModule);

	memset(&data, 0, sizeof(data));
	data.flags=DB_DBT_PARTIAL | DB_DBT_USERMEM;
	
	while (DBOK(nRC) )
	{
		memset(sModule, 0, sizeof(sModule));

		nRC = pDBC->get(pDBC, &key, &data, DB_NEXT);

		if ( DBOK(nRC) )
		{
			if (fnmatch(psBuffer, sModule, FNM_EXTMATCH | FNM_CASEFOLD) == 0)
			{
				pControl->psModules[pControl->nModules]=_strdup(sModule);
				pControl->nModules++;
			}
		}
	}
	
	pDBC->close(pDBC);

	return 1;
}

int AddChange(struct DBCONTROL *pControl, char* psBuffer)
{
	DBT					key, data;
	int					nRC;
	unsigned int		i;
	time_t				tCurrent;
	char				sBuffer[2048];

	if ( strlen(psBuffer) < 2 )
	{
		return 0;
	}

	DBDBTINIT(data, sizeof(tCurrent), &tCurrent);
	tCurrent = time(NULL);
	
	memset(&key, 0, sizeof(key));
	key.data=sBuffer;
	
	for(i=0; i < pControl->nModules; i++)
	{
		key.size = sprintf(sBuffer, "%s;%c;%s",
			pControl->psModules[i],
			*psBuffer,
			(char*)psBuffer+1);

		nRC = pControl->pDBChanges->put(pControl->pDBChanges, NULL, &key, &data, DB_NOOVERWRITE);

		if (DBOK(nRC))
		{
			fprintf(stderr, "New Record: %s\n", sBuffer);
		}
	}
	
	return 1;
}

struct CHANGEITEM {
	char*   psModule;
	time_t	tTime;
	char*	psNote;
	char	ctype;
};

int ListSort(const void* arg1, const void* arg2)
{
	struct CHANGEITEM* pArg1 = (struct CHANGEITEM*)arg1;
	struct CHANGEITEM* pArg2 = (struct CHANGEITEM*)arg2;
	int	               nRC;
	
	nRC = strcmp(pArg1->psModule, pArg2->psModule);

	if ( nRC )
		return nRC;

	nRC = (int)difftime(pArg2->tTime, pArg1->tTime);
	
	return nRC;
}

int ListChange(struct DBCONTROL *pControl, char* psSearch)
{
	DBT							key, data;
	int							nRC;
	unsigned int				i;
	DBC							*pDBC;
	time_t						tTime;
	char						sBuffer[2048];
	struct CHANGEITEM			*pList;
	unsigned int				nListCount=0;
	char*						pModule;
	char*						pType;
	char*						pNote;
	struct CHANGEITEM			*pListLast;
	struct CHANGEITEM			*pListCurrent;
	struct ESPREPORT_CONTROL	oReport;
	struct tm					*pTM;
	char*                       psType = "Undefined";
	int64_t						longtime;
	size_t						nLength;
	
	nRC = pControl->pDBChanges->cursor(pControl->pDBChanges, NULL, &pDBC, 0);

	if ( DBNOTOK(nRC) )
	{
		return 0;
	}
		
	pList = (struct CHANGEITEM*)calloc(1000000, sizeof(struct CHANGEITEM));

	DBDBTINIT(key, sizeof(sBuffer), &sBuffer);
	DBDBTINIT(data, sizeof(longtime), &longtime);

	while (DBOK(nRC) )
	{
		memset(sBuffer, 0, sizeof(sBuffer));

		nRC = pDBC->get(pDBC, &key, &data, DB_NEXT);
		
		tTime = (time_t)longtime;

		if ( DBOK(nRC) )
		{
			if (fnmatch(psSearch, sBuffer, FNM_EXTMATCH | FNM_CASEFOLD) == 0)
			{
				pList[nListCount].tTime = tTime;
				pModule = strtok(sBuffer, ";");
				pType = strtok(NULL, ";");
				pNote = strtok(NULL, ";");
				pList[nListCount].psModule = _strdup(pModule);
				pList[nListCount].ctype = *pType;
				pList[nListCount].psNote = _strdup(pNote);
				nListCount++;
			}
		}
	}
	
	pDBC->close(pDBC);

	qsort(pList, nListCount, sizeof(struct CHANGEITEM), ListSort);

	pListCurrent = pList;
	pListLast = NULL;

	ESPReport_Init(&oReport, NULL);
	oReport.nColumnOffset++;
	oReport.addcolumn(&oReport, 10, "Date", 0);
	oReport.addcolumn(&oReport, 11, "Type", 0);
	oReport.addcolumn(&oReport, 150, "Details", 0);

	for (i=0; i < nListCount; i++, pListCurrent++)
	{
		if ( pListLast == NULL || strcmp(pListLast->psModule, pListCurrent->psModule) )
		{
			oReport.printheader(&oReport, NULL, "Changelog", "Module(%s)", pListCurrent->psModule);
		}

		pTM = localtime(&pListCurrent->tTime);
		strftime(sBuffer, sizeof(sBuffer), "%Y-%m-%d",  pTM);

		switch( pListCurrent->ctype )
		{
		case '!':
			psType = "Fix";
			break;
		case '+':
			psType = "Enhancement";
			break;
		default:
			psType = "Undefined";
			break;
		}

		nLength = strlen(pListCurrent->psNote);

		if (pListCurrent->psNote[nLength - 1] == '.')
		{
			pListCurrent->psNote[nLength - 1] = 0x00;
		}

		oReport.print(&oReport, 
			"%s|%s|%s",
			sBuffer,
			psType,
			pListCurrent->psNote);

		pListLast = pListCurrent;
	}

	//*
	//* Free the list
	//*
	for (i=0; i < nListCount; i++)
	{
		free(pList[i].psModule);
		free(pList[i].psNote);
	}

	free(pList);

	oReport.term(&oReport);

	return 1;
}

int ListChangeRSS(struct DBCONTROL *pControl, char* psModule)
{
	DBT							key, data;
	int							nRC;
	unsigned int				i;
	DBC							*pDBC;
	time_t						tTime;
	char						sBuffer[2048];
	struct CHANGEITEM			*pList;
	unsigned int				nListCount = 0;
	char*						pModule;
	char*						pType;
	char*						pNote;
	struct CHANGEITEM			*pListLast;
	struct CHANGEITEM			*pListCurrent;
	struct tm					*pTM;
	char*                       psType = "Undefined";
	int64_t						longtime;
	size_t						nLength;

	fprintf(stdout, "<?xml version = \"1.0\" ?>\n");
	fprintf(stdout, "<rss version = \"2.0\">\n"); 
	fprintf(stdout, "\t<channel>\n");
	fprintf(stdout, "\t\t<title>TapeTrack Change Log RSS</title>\n");
	fprintf(stdout, "\t\t<description>TapeTrack Change Log></description>\n");

	nRC = pControl->pDBChanges->cursor(pControl->pDBChanges, NULL, &pDBC, 0);

	if (DBNOTOK(nRC))
	{
		return 0;
	}

	pList = (struct CHANGEITEM*)calloc(1000000, sizeof(struct CHANGEITEM));

	DBDBTINIT(key, sizeof(sBuffer), &sBuffer);
	DBDBTINIT(data, sizeof(longtime), &longtime);

	while (DBOK(nRC))
	{
		memset(sBuffer, 0, sizeof(sBuffer));

		nRC = pDBC->get(pDBC, &key, &data, DB_NEXT);
		
		tTime = (time_t)longtime;

		if (DBOK(nRC))
		{
			pModule = strtok(sBuffer, ";");

			if ( !psModule || (strcmp(psModule, pModule)==0))
			{
				pList[nListCount].tTime = tTime;
				pType = strtok(NULL, ";");
				pNote = strtok(NULL, ";");
				pList[nListCount].psModule = _strdup(pModule);
				pList[nListCount].ctype = *pType;
				pList[nListCount].psNote = _strdup(pNote);
				nListCount++;
			}
		}
	}

	pDBC->close(pDBC);

	qsort(pList, nListCount, sizeof(struct CHANGEITEM), ListSort);

	pListCurrent = pList;
	pListLast = NULL;

	for (i = 0; i < nListCount; i++, pListCurrent++)
	{
		if (pListLast == NULL || strcmp(pListLast->psModule, pListCurrent->psModule))
		{
		}

		pTM = gmtime(&pListCurrent->tTime);
		if (pTM)
		{
			strftime(sBuffer, sizeof(sBuffer), "%a, %d %b %Y %H:%M:%S GMT", pTM);
		}

		switch (pListCurrent->ctype)
		{
		case '!':
			psType = "Fix";
			break;
		case '+':
			psType = "Enhancement";
			break;
		default:
			psType = "Undefined";
			break;
		}

		nLength = strlen(pListCurrent->psNote);

		if (pListCurrent->psNote[nLength - 1] == '.')
		{
			pListCurrent->psNote[nLength - 1] = 0x00;
		}

		fprintf(stdout, "\t\t<item>\n");
		fprintf(stdout, "\t\t\t<title>%s: %s (%s)</title>\n", psType, pListCurrent->psModule, sBuffer);
		fprintf(stdout, "\t\t\t<description>%s</description>\n", pListCurrent->psNote);
		
		fprintf(stdout, "\t\t\t<pubDate>%s</pubDate>\n", sBuffer);
		fprintf(stdout, "\t\t</item>\n"); 

		pListLast = pListCurrent;
	}

	//*
	//* Free the list
	//*
	for (i = 0; i < nListCount; i++)
	{
		free(pList[i].psModule);
		free(pList[i].psNote);
	}

	free(pList);
	

	fprintf(stdout, "\t</channel>\n");
	fprintf(stdout, "</rss>\n");

	return 1;
}

int ListChangeDocbook(struct DBCONTROL *pControl, char* psSearch)
{
	DBT							key, data;
	int							nRC;
	unsigned int				i;
	DBC							*pDBC;
	time_t						tTime;
	char						sBuffer[2048];
	struct CHANGEITEM			*pList;
	unsigned int				nListCount = 0;
	char*						pModule;
	char*						pType;
	char*						pNote;
	struct CHANGEITEM			*pListLast;
	struct CHANGEITEM			*pListCurrent;
	struct tm					*pTM;
	char*                       psType = "Undefined";
	int64_t						longtime;
	int							nLength;

	fprintf(stdout, "   <variablelist xmlns=\"http://docbook.org/ns/docbook\" version=\"5.0\">\n");

	nRC = pControl->pDBChanges->cursor(pControl->pDBChanges, NULL, &pDBC, 0);

	if (DBNOTOK(nRC))
	{
		return 0;
	}

	pList = (struct CHANGEITEM*)calloc(1000000, sizeof(struct CHANGEITEM));

	DBDBTINIT(key, sizeof(sBuffer), &sBuffer);
	DBDBTINIT(data, sizeof(longtime), &longtime);

	while (DBOK(nRC))
	{
		memset(sBuffer, 0, sizeof(sBuffer));

		nRC = pDBC->get(pDBC, &key, &data, DB_NEXT);

		tTime = (time_t)longtime;

		if (DBOK(nRC))
		{
			if (fnmatch(psSearch, sBuffer, FNM_EXTMATCH | FNM_CASEFOLD) == 0)
			{
				pList[nListCount].tTime = tTime;
				pModule = strtok(sBuffer, ";");
				pType = strtok(NULL, ";");
				pNote = strtok(NULL, ";");
				pList[nListCount].psModule = _strdup(pModule);
				pList[nListCount].ctype = *pType;
				pList[nListCount].psNote = _strdup(pNote);
				nListCount++;
			}
		}
	}

	pDBC->close(pDBC);

	qsort(pList, nListCount, sizeof(struct CHANGEITEM), ListSort);

	pListCurrent = pList;
	pListLast = NULL;

	for (i = 0; i < nListCount; i++, pListCurrent++)
	{
		if (pListLast == NULL || strcmp(pListLast->psModule, pListCurrent->psModule))
		{
		}

		pTM = localtime(&pListCurrent->tTime);
		if (pTM)
		{
			strftime(sBuffer, sizeof(sBuffer), "%Y-%m-%d", pTM);
		}

		switch (pListCurrent->ctype)
		{
		case '!':
			psType = "Fix";
			break;
		case '+':
			psType = "Enhancement";
			break;
		default:
			psType = "Undefined";
			break;
		}
			
		nLength = strlen(pListCurrent->psNote);

		if (pListCurrent->psNote[nLength - 1] == '.')
		{
			pListCurrent->psNote[nLength - 1] = 0x00;
		}
		
		fprintf(stdout, "      <varlistentry>\n");
		fprintf(stdout, "         <term><emphasis role=\"bold\">%s</emphasis></term>\n", sBuffer);
		fprintf(stdout, "         <listitem><para><emphasis role=\"bold\">%s</emphasis> %s.</para></listitem>\n", psType, pListCurrent->psNote);
		fprintf(stdout, "      </varlistentry>\n");

		pListLast = pListCurrent;
	}

	//*
	//* Free the list
	//*
	for (i = 0; i < nListCount; i++)
	{
		free(pList[i].psModule);
		free(pList[i].psNote);
	}

	free(pList);


	fprintf(stdout, "   </variablelist>\n");

	return 1;
}
