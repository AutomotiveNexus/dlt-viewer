/**
 * @licence app begin@
 * Copyright (C) 2011-2012  BMW AG
 *
 * This file is part of GENIVI Project Dlt Viewer.
 *
 * Contributions are licensed to the GENIVI Alliance under one or more
 * Contribution License Agreements.
 *
 * \copyright
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed with
 * this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * \file mainwindow.cpp
 * For further information see http://www.genivi.org/.
 * @licence end@
 */

#include <iostream>
#include <QMimeData>
#include <QTreeView>
#include <QFileDialog>
#include <QProgressDialog>
#include <QTemporaryFile>
#include <QPluginLoader>
#include <QPushButton>
#include <QKeyEvent>
#include <QClipboard>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QLineEdit>
#include <QUrl>
#include <QDateTime>
#include <QLabel>
#include <QInputDialog>
#include <QByteArray>

/**
 * From QDlt.
 * Must be a "C" include to interpret the imports correctly
 * for MSVC compilers.
 **/
extern "C" {
    #include "dlt_common.h"
    #include "dlt_user.h"
}

#include <unistd.h>     /* for read(), close() */
#include <sys/time.h>	/* for gettimeofday() */

#include "mainwindow.h"

#include "ecudialog.h"
#include "applicationdialog.h"
#include "contextdialog.h"
#include "multiplecontextdialog.h"
#include "plugindialog.h"
#include "settingsdialog.h"
#include "injectiondialog.h"
#include "qextserialenumerator.h"
#include "version.h"
#include "dltfileutils.h"
#include "dltuiutils.h"
#include "dltexporter.h"
#include "jumptodialog.h"
#include "fieldnames.h"
#include "tablemodel.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    timer(this),
    qcontrol(this),
    pulseButtonColor(255, 40, 40)
{
    ui->setupUi(this);
    ui->enableConfigFrame->setVisible(false);
    setAcceptDrops(true);

    initState();

    /* Apply loaded settings */
    initSearchTable();

    initView();
    applySettings();

    initSignalConnections();

    initFileHandling();

    // check and clear index cache if needed
    settings->clearIndexCacheAfterDays();

    /* Command plugin */
    if(OptManager::getInstance()->isPlugin())
    {
        commandLineExecutePlugin(OptManager::getInstance()->getPluginName(),
                                 OptManager::getInstance()->getCommandName(),
                                 OptManager::getInstance()->getCommandParams());
    }

    /* auto connect */
    if(settings->autoConnect)
    {
        connectAll();
    }

    /* start timer for autoconnect */
    connect(&timer, SIGNAL(timeout()), this, SLOT(timeout()));
    timer.start(1000);

    restoreGeometry(DltSettingsManager::getInstance()->value("geometry").toByteArray());
    restoreState(DltSettingsManager::getInstance()->value("windowState").toByteArray());
}

MainWindow::~MainWindow()
{
    DltSettingsManager::close();
    /**
     * All plugin dockwidgets must be removed from the layout manually and
     * then deleted. This has to be done here, because they contain
     * UI components owned by the plugins. The plugins will destroy their
     * own UI components. If the dockwidget is not manually removed, the
     * parent destructor of MainWindow will try to automatically delete
     * the dockWidgets subcomponents, which are already destroyed
     * when unloading plugins.
     **/
    for(int i=0;i<project.plugin->topLevelItemCount();i++)
    {
        PluginItem *item = (PluginItem *) project.plugin->topLevelItem(i);
        if(item->dockWidget != NULL)
        {
            removeDockWidget(item->dockWidget);
            delete item->dockWidget;
        }
    }

    // rename output filename if flag set in settings
    if(settings->appendDateTime)
    {
        // get new filename
        QFileInfo info(outputfile.fileName());
        QString newFilename = info.baseName()+
                (startLoggingDateTime.toString("__yyyyMMdd_hhmmss"))+
                (QDateTime::currentDateTime().toString("__yyyyMMdd_hhmmss"))+
                QString(".dlt");
        QFileInfo infoNew(info.absolutePath(),newFilename);

        // rename old file
        outputfile.rename(infoNew.absoluteFilePath());
    }

    delete ui;
    delete tableModel;
    delete searchDlg;
    delete dltIndexer;
    delete m_shortcut_searchnext;
    delete m_shortcut_searchprev;
}

void MainWindow::initState()
{

    /* Settings */
    settings = new SettingsDialog(&qfile,this);
    settings->assertSettingsVersion();
    settings->readSettings();
    recentFiles = settings->getRecentFiles();
    recentProjects = settings->getRecentProjects();
    recentFilters = settings->getRecentFilters();

    /* Initialize recent files */
    for (int i = 0; i < MaxRecentFiles; ++i) {
        recentFileActs[i] = new QAction(this);
        recentFileActs[i]->setVisible(false);
        connect(recentFileActs[i], SIGNAL(triggered()), this, SLOT(openRecentFile()));
        ui->menuRecent_files->addAction(recentFileActs[i]);
    }

    /* Initialize recent projects */
    for (int i = 0; i < MaxRecentProjects; ++i) {
        recentProjectActs[i] = new QAction(this);
        recentProjectActs[i]->setVisible(false);
        connect(recentProjectActs[i], SIGNAL(triggered()), this, SLOT(openRecentProject()));
        ui->menuRecent_projects->addAction(recentProjectActs[i]);
    }

    /* Initialize recent filters */
    for (int i = 0; i < MaxRecentFilters; ++i) {
        recentFiltersActs[i] = new QAction(this);
        recentFiltersActs[i]->setVisible(false);
        connect(recentFiltersActs[i], SIGNAL(triggered()), this, SLOT(openRecentFilters()));
        ui->menuRecent_Filters->addAction(recentFiltersActs[i]);
    }

    /* Update recent file and project actions */
    updateRecentFileActions();
    updateRecentProjectActions();
    updateRecentFiltersActions();

    /* initialise DLT file handling */
    tableModel = new TableModel("Hello Tree");
    tableModel->qfile = &qfile;
    tableModel->project = &project;
    tableModel->pluginManager = &pluginManager;

    /* initialise project configuration */
    project.ecu = ui->configWidget;
    project.filter = ui->filterWidget;
    project.plugin = ui->pluginWidget;
    project.settings = settings;

    /* Load Plugins before loading default project */
    loadPlugins();
    pluginManager.autoscrollStateChanged(settings->autoScroll);

    /* initialize injection */
    injectionAplicationId.clear();
    injectionContextId.clear();
    injectionServiceId.clear();
    injectionData.clear();
    injectionDataBinary = false;
}

void MainWindow::initView()
{

    /* update default filter selection */
    ui->comboBoxFilterSelection->addItem("<No filter selected>");
    on_actionDefault_Filter_Reload_triggered();

    /* set table size and en */
    ui->tableView->setModel(tableModel);

    /* For future use enable HTML View in Table */
    //HtmlDelegate* delegate = new HtmlDelegate();
    //ui->tableView->setItemDelegate(delegate);
    //ui->tableView->setItemDelegateForColumn(FieldNames::Payload,delegate);

    ui->tableView->setColumnWidth(0,50);
    ui->tableView->setColumnWidth(1,150);
    ui->tableView->setColumnWidth(2,70);
    ui->tableView->setColumnWidth(3,40);
    ui->tableView->setColumnWidth(4,40);
    ui->tableView->setColumnWidth(5,40);
    ui->tableView->setColumnWidth(6,40);
    ui->tableView->setColumnWidth(7,50);
    ui->tableView->setColumnWidth(8,50);
    ui->tableView->setColumnWidth(9,50);
    ui->tableView->setColumnWidth(10,40);
    ui->tableView->setColumnWidth(11,40);
    ui->tableView->setColumnWidth(12,400);

    /* Enable column sorting of config widget */
    ui->configWidget->sortByColumn(0, Qt::AscendingOrder); // column/order to sort by
    ui->configWidget->setSortingEnabled(true);             // should cause sort on add
    ui->configWidget->setHeaderHidden(false);
    ui->filterWidget->setHeaderHidden(false);
    ui->pluginWidget->setHeaderHidden(false);

    /* Start pulsing the apply changes button, when filters draged&dropped */
    connect(ui->filterWidget, SIGNAL(filterItemDropped()), this, SLOT(filterOrderChanged()));

    /* initialise statusbar */
    totalBytesRcvd = 0;
    totalByteErrorsRcvd = 0;
    totalSyncFoundRcvd = 0;
    statusFilename = new QLabel("no log file loaded");
    statusFileVersion = new QLabel("Version: <unknown>");
    statusBytesReceived = new QLabel("Recv: 0");
    statusByteErrorsReceived = new QLabel("Recv Errors: 0");
    statusSyncFoundReceived = new QLabel("Sync found: 0");
    statusProgressBar = new QProgressBar();
    statusBar()->addWidget(statusFilename);
    statusBar()->addWidget(statusFileVersion);
    statusBar()->addWidget(statusBytesReceived);
    statusBar()->addWidget(statusByteErrorsReceived);
    statusBar()->addWidget(statusSyncFoundReceived);
    statusBar()->addWidget(statusProgressBar);

    /* Create search text box */
    searchTextbox = new QLineEdit();
    searchDlg->appendLineEdit(searchTextbox);
    connect(searchTextbox, SIGNAL(textEdited(QString)),searchDlg,SLOT(textEditedFromToolbar(QString)));
    connect(searchTextbox, SIGNAL(returnPressed()),searchDlg,SLOT(findNextClicked()));

    /* Initialize toolbars. Most of the construction and connection is done via the
     * UI file. See mainwindow.ui, ActionEditor and Signal & Slots editor */
    QList<QAction *> mainActions = ui->mainToolBar->actions();
    m_searchActions = ui->searchToolbar->actions();

    /* Point scroll toggle button to right place */
    scrollButton = mainActions.at(ToolbarPosition::AutoScroll);

    /* Update the scrollbutton status */
    updateScrollButton();
}

void MainWindow::initSignalConnections()
{
    /* Connect RegExp settings from and to search dialog */
    connect(m_searchActions.at(ToolbarPosition::Regexp), SIGNAL(toggled(bool)), searchDlg->regexpCheckBox, SLOT(setChecked(bool)));
    connect(searchDlg->regexpCheckBox, SIGNAL(toggled(bool)), m_searchActions.at(ToolbarPosition::Regexp), SLOT(setChecked(bool)));

    /* Connect previous and next buttons to search dialog slots */
    connect(m_searchActions.at(ToolbarPosition::FindPrevious), SIGNAL(triggered()), searchDlg, SLOT(findPreviousClicked()));
    connect(m_searchActions.at(ToolbarPosition::FindNext), SIGNAL(triggered()), searchDlg, SLOT(findNextClicked()));

    connect(searchDlg->CheckBoxSearchtoList,SIGNAL(toggled(bool)),ui->actionSearchList,SLOT(setChecked(bool)));
    connect(ui->actionSearchList,SIGNAL(toggled(bool)),searchDlg->CheckBoxSearchtoList,SLOT(setChecked(bool)));
    ui->actionSearchList->setChecked(searchDlg->searchtoIndex());


    /* Insert search text box to search toolbar, before previous button */
    QAction *before = m_searchActions.at(ToolbarPosition::FindPrevious);
    ui->searchToolbar->insertWidget(before, searchTextbox);

    /* adding shortcuts - regard: in the search window, the signal is caught by another way, this here only catches the keys when main window is active */
    m_shortcut_searchnext = new QShortcut(QKeySequence("F3"), this);
    connect(m_shortcut_searchnext, SIGNAL(activated()), searchDlg, SLOT( on_pushButtonNext_clicked() ) );
    m_shortcut_searchprev = new QShortcut(QKeySequence("F2"), this);
    connect(m_shortcut_searchprev, SIGNAL(activated()), searchDlg, SLOT( on_pushButtonPrevious_clicked() ) );

    connect((QObject*)(ui->tableView->verticalScrollBar()), SIGNAL(valueChanged(int)), this, SLOT(tableViewValueChanged(int)));
    connect(ui->tableView->horizontalHeader(), SIGNAL(sectionDoubleClicked(int)), this, SLOT(sectionInTableDoubleClicked(int)));

    //for search result table
    connect(searchDlg, SIGNAL(refreshedSearchIndex()), this, SLOT(searchTableRenewed()));
    connect( m_searchresultsTable, SIGNAL( doubleClicked (QModelIndex) ), this, SLOT( searchtable_cellSelected( QModelIndex ) ) );

    // connect tableView selection model change to handler in mainwindow
    connect(ui->tableView->selectionModel(),
            SIGNAL(selectionChanged(const QItemSelection &, const QItemSelection &)),
            SLOT(on_tableView_selectionChanged(const QItemSelection &, const QItemSelection &)));

}

void MainWindow::initSearchTable()
{

    //init search Dialog
    searchDlg = new SearchDialog(this);
    searchDlg->file = &qfile;
    searchDlg->table = ui->tableView;
    searchDlg->pluginManager = &pluginManager;

    /* initialise DLT Search handling */
    m_searchtableModel = new SearchTableModel("Search Index Mainwindow");
    m_searchtableModel->qfile = &qfile;
    m_searchtableModel->project = &project;
    m_searchtableModel->pluginManager = &pluginManager;

    searchDlg->registerSearchTableModel(m_searchtableModel);

    m_searchresultsTable = ui->tableView_SearchIndex;
    m_searchresultsTable->setModel(m_searchtableModel);

    m_searchresultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);


    m_searchresultsTable->verticalHeader()->setVisible(false);    
    m_searchresultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    //Removing lines which are unlinkely to be necessary for a search. Maybe make configurable.
    //Ideally possible with right-click
    m_searchresultsTable->setColumnHidden(FieldNames::Counter, true);
    m_searchresultsTable->setColumnHidden(FieldNames::Type, true);
    m_searchresultsTable->setColumnHidden(FieldNames::Subtype, true);
    m_searchresultsTable->setColumnHidden(FieldNames::Mode, true);
    m_searchresultsTable->setColumnHidden(FieldNames::ArgCount, true);
    m_searchresultsTable->setColumnHidden(FieldNames::SessionId, true);

    QFont searchtableViewFont = m_searchresultsTable->font();
    searchtableViewFont.setPointSize(settings->fontSize);
    m_searchresultsTable->setFont(searchtableViewFont);

    // Rescale the height of a row to choosen font size + 8 pixels
    m_searchresultsTable->verticalHeader()->setDefaultSectionSize(settings->fontSize+8);

    /* set table size and en */
    m_searchresultsTable->setColumnWidth(FieldNames::Index,50);
    m_searchresultsTable->setColumnWidth(FieldNames::Time,150);
    m_searchresultsTable->setColumnWidth(FieldNames::TimeStamp,70);
    m_searchresultsTable->setColumnWidth(FieldNames::Counter,40);
    m_searchresultsTable->setColumnWidth(FieldNames::EcuId,40);
    m_searchresultsTable->setColumnWidth(FieldNames::AppId,40);
    m_searchresultsTable->setColumnWidth(FieldNames::ContextId,40);
    m_searchresultsTable->setColumnWidth(FieldNames::SessionId,50);
    m_searchresultsTable->setColumnWidth(FieldNames::Type,50);
    m_searchresultsTable->setColumnWidth(FieldNames::Subtype,50);
    m_searchresultsTable->setColumnWidth(FieldNames::Mode,40);
    m_searchresultsTable->setColumnWidth(FieldNames::ArgCount,40);
    m_searchresultsTable->setColumnWidth(FieldNames::Payload,1000);

    ui->dockWidgetSearchIndex->hide();    

}

void MainWindow::initFileHandling()
{
    /* Initialize dlt-file indexer  */
    dltIndexer = new DltFileIndexer(&qfile,&pluginManager,&defaultFilter, this);

    /* connect signals */
    connect(dltIndexer, SIGNAL(progressMax(quint64)), this, SLOT(reloadLogFileProgressMax(quint64)));
    connect(dltIndexer, SIGNAL(progress(quint64)), this, SLOT(reloadLogFileProgress(quint64)));
    connect(dltIndexer, SIGNAL(progressText(QString)), this, SLOT(reloadLogFileProgressText(QString)));
    connect(dltIndexer, SIGNAL(versionString(QString,QString)), this, SLOT(reloadLogFileVersionString(QString,QString)));
    connect(dltIndexer, SIGNAL(finishIndex()), this, SLOT(reloadLogFileFinishIndex()));
    connect(dltIndexer, SIGNAL(finishFilter()), this, SLOT(reloadLogFileFinishFilter()));
    connect(dltIndexer, SIGNAL(finishDefaultFilter()), this, SLOT(reloadLogFileFinishDefaultFilter()));
    connect(dltIndexer, SIGNAL(timezone(int,unsigned char)), this, SLOT(controlMessage_Timezone(int,unsigned char)));
    connect(dltIndexer, SIGNAL(unregisterContext(QString,QString,QString)), this, SLOT(controlMessage_UnregisterContext(QString,QString,QString)));

    /* Plugins/Filters enabled checkboxes */
    ui->pluginsEnabled->setChecked(DltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool());
    ui->filtersEnabled->setChecked(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool());
    ui->checkBoxSortByTime->setEnabled(ui->filtersEnabled->isChecked());
    ui->checkBoxSortByTime->setChecked(DltSettingsManager::getInstance()->value("startup/sortByTimeEnabled", false).toBool());

    /* Process Project */
    if(OptManager::getInstance()->isProjectFile())
    {
        openDlpFile(OptManager::getInstance()->getProjectFile());

    } else {
        /* Load default project file */
        this->setWindowTitle(QString("DLT Viewer - unnamed project - Version : %1 %2").arg(PACKAGE_VERSION).arg(PACKAGE_VERSION_STATE));
        if(settings->defaultProjectFile)
        {
            if(!openDlpFile(settings->defaultProjectFileName)){
                QMessageBox::critical(0, QString("DLT Viewer"),
                                      QString("Cannot load default project \"%1\"")
                                      .arg(settings->defaultProjectFileName));
            }
        }
    }

    /* Process Logfile */
    outputfileIsFromCLI = false;
    outputfileIsTemporary = false;
    if(OptManager::getInstance()->isLogFile())
    {
        openDltFile(QStringList(OptManager::getInstance()->getLogFile()));
        /* Command line file is treated as temp file */
        outputfileIsTemporary = true;
        outputfileIsFromCLI = true;
    }
    else
    {
        /* load default log file */
        statusFilename->setText("no log file loaded");
        if(settings->defaultLogFile)
        {
            openDltFile(QStringList(settings->defaultLogFileName));
            outputfileIsFromCLI = false;
            outputfileIsTemporary = false;
        }
        else
        {
            /* Create temp file */
            QString fn = DltFileUtils::createTempFile(DltFileUtils::getTempPath(settings));
            outputfile.setFileName(fn);
            outputfileIsTemporary = true;
            outputfileIsFromCLI = false;
            if(outputfile.open(QIODevice::WriteOnly|QIODevice::Truncate))
            {
                openFileNames = QStringList(fn);
                reloadLogFile();
            }
            else
                QMessageBox::critical(0, QString("DLT Viewer"),
                                      QString("Cannot load temporary log file \"%1\"\n%2")
                                      .arg(outputfile.fileName())
                                      .arg(outputfile.errorString()));
        }
    }

    if(OptManager::getInstance()->isFilterFile()){
        if(project.LoadFilter(OptManager::getInstance()->getFilterFile(),false))
        {
            filterUpdate();
            setCurrentFilters(OptManager::getInstance()->getFilterFile());

        }
    }
    if(OptManager::getInstance()->isConvert())
    {
        commandLineConvertToASCII();
        exit(0);
    }


	draw_timer.setSingleShot (true);
    connect(&draw_timer, SIGNAL(timeout()), this, SLOT(draw_timeout()));


    DltSettingsManager *settingsmanager = DltSettingsManager::getInstance();
    bool startup_minimized = settingsmanager->value("StartUpMinimized",false).toBool();
    if (startup_minimized)
        this->setWindowState(Qt::WindowMinimized);
}

void MainWindow::commandLineConvertToASCII()
{

    qfile.enableFilter(true);
    openDltFile(QStringList(OptManager::getInstance()->getConvertSourceFile()));
    outputfileIsFromCLI = false;
    outputfileIsTemporary = false;

    QFile asciiFile(OptManager::getInstance()->getConvertDestFile());

    /* start exporter */
    DltExporter exporter;
    exporter.exportMessages(&qfile,&asciiFile,&pluginManager,DltExporter::FormatAscii,DltExporter::SelectionFiltered);
}

void MainWindow::ErrorMessage(QMessageBox::Icon level, QString title, QString message){

  if (OptManager::getInstance()->issilentMode())
    {
      qDebug()<<message;
    }
  else
    {
      if (level == QMessageBox::Critical)
        QMessageBox::critical(this, title, message);
      else if (level == QMessageBox::Warning)
        QMessageBox::warning(this, title, message);
      else if (level == QMessageBox::Information)
        QMessageBox::information(this, title, message);
      else
        QMessageBox::critical(this, "ErrorMessage problem", "unhandled case");
    }

}

void MainWindow::commandLineExecutePlugin(QString name, QString cmd, QStringList params)
{
    QDltPlugin *plugin = pluginManager.findPlugin(name);

    if(!plugin)
    {
        qDebug() << "Plugin not found " << plugin;
        return;

    }

    /* Check that this is a command plugin */
    if(!plugin->isCommand())
    {
        QString msg("Error: ");
        msg = msg+name+" is not a command plugin.";
        ErrorMessage(QMessageBox::Critical, name, msg);
        exit(-1);
    }
    if(!plugin->command(cmd, params))
    {
        QString msg("Error: ");
        msg.append(name);
        msg.append(plugin->error());
        ErrorMessage(QMessageBox::Warning,name, msg);

        exit(-1);
    }
    else
    {
        exit(0);
    }

}

void MainWindow::deleteactualFile(){
    if(outputfileIsTemporary && !outputfileIsFromCLI)
    {
        // Delete created temp file
        qfile.close();
        outputfile.close();
        if(outputfile.exists() && !outputfile.remove())
        {
            QMessageBox::critical(0, QString("DLT Viewer"),
                                  QString("Cannot delete temporary log file \"%1\"\n%2")
                                  .arg(outputfile.fileName())
                                  .arg(outputfile.errorString()));
        }
    }
}


void MainWindow::closeEvent(QCloseEvent *event)
{

    settings->writeSettings(this);
    if(settings->tempCloseWithoutAsking || outputfile.size() == 0)
    {

        deleteactualFile();

        QMainWindow::closeEvent(event);
    }
    else if(outputfileIsTemporary && !outputfileIsFromCLI)
    {
        if(QMessageBox::information(this, "DLT Viewer",
           "You still have an unsaved temporary file open. Exit anyway?",
           QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
        {
            deleteactualFile();

            QMainWindow::closeEvent(event);
        }
        else
        {
            event->ignore();
        }
    }
    else
    {
        QMainWindow::closeEvent(event);
    }
}

void MainWindow::on_action_menuFile_New_triggered()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("New DLT Log file"), workingDirectory.getDltDirectory(), tr("DLT Files (*.dlt);;All files (*.*)"));

    if(fileName.isEmpty())
    {
        return;
    }

    on_New_triggered(fileName);
}

void MainWindow::on_New_triggered(QString fileName)
{

    /* change DLT file working directory */
    workingDirectory.setDltDirectory(QFileInfo(fileName).absolutePath());

    /* close existing file */
    if(outputfile.isOpen())
    {
        if (outputfile.size() == 0)
        {
            deleteactualFile();
        }
        else
        {
            outputfile.close();
        }
    }

    /* create new file; truncate if already exist */
    outputfile.setFileName(fileName);
    outputfileIsTemporary = false;
    outputfileIsFromCLI = false;
    setCurrentFile(fileName);
    if(outputfile.open(QIODevice::WriteOnly|QIODevice::Truncate))
    {
        openFileNames = QStringList(fileName);
        reloadLogFile();
    }
    else
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("Cannot create new log file \"%1\"\n%2")
                              .arg(fileName)
                              .arg(outputfile.errorString()));
}

void MainWindow::on_action_menuFile_Open_triggered()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this,
        tr("Open one or more DLT Log files"), workingDirectory.getDltDirectory(), tr("DLT Files (*.dlt);;All files (*.*)"));

    if(fileNames.isEmpty())
        return;

    on_Open_triggered(fileNames);
}

void MainWindow::on_Open_triggered(QStringList filenames)
{

    /* change DLT file working directory */
    workingDirectory.setDltDirectory(QFileInfo(filenames[0]).absolutePath());

    openDltFile(filenames);
    outputfileIsFromCLI = false;
    outputfileIsTemporary = false;

    searchDlg->setMatch(false);
    searchDlg->setOnceClicked(false);
    searchDlg->setStartLine(-1);
}


void MainWindow::openRecentFile()
{
    QAction *action = qobject_cast<QAction *>(sender());
    QString fileName;
    if (action)
    {
        fileName = action->data().toString();

        if(fileName.isEmpty())
        {
            removeCurrentFile(fileName);
            return;
        }
        workingDirectory.setDltDirectory(QFileInfo(fileName).absolutePath());

         /* open existing file and append new data */
        if (true == openDltFile(QStringList(fileName)))
          {
            outputfileIsTemporary = false;
            outputfileIsFromCLI = false;
          }
        else
          {
            removeCurrentFile(fileName);
          }
    }
}

bool MainWindow::openDltFile(QStringList fileNames)
{
    /* close existing file */
    bool ret = false;

    if(fileNames.size()==0)
        return false;

    if(outputfile.isOpen())
    {
        if (outputfile.size() == 0)
        {
            deleteactualFile();
        }
        else
        {
            outputfile.close();
        }
    }

    /* open existing file and append new data */
    outputfile.setFileName(fileNames.last());
    setCurrentFile(fileNames.last());
    if(outputfile.open(QIODevice::WriteOnly|QIODevice::Append))
    {
        openFileNames = fileNames;
        if(OptManager::getInstance()->isConvert() || OptManager::getInstance()->isPlugin())
            // if dlt viewer started as converter or with plugin option load file non multithreaded
            reloadLogFile(false,false);
        else
            // normally load log file mutithreaded
            reloadLogFile();
        ret = true;
    }
    else
    {
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("Cannot open log file \"%1\"\n%2")
                              .arg(fileNames.last())
                              .arg(outputfile.errorString()));
        ret = false;

    }

    return ret;
}

void MainWindow::on_action_menuFile_Import_DLT_Stream_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Import DLT Stream"), workingDirectory.getDltDirectory(), tr("DLT Stream file (*.*)"));

    if(fileName.isEmpty())
        return;

    /* change DLT file working directory */
    workingDirectory.setDltDirectory(QFileInfo(fileName).absolutePath());

    if(!outputfile.isOpen())
        return;

    DltFile importfile;

    dlt_file_init(&importfile,0);

    /* open DLT stream file */
    dlt_file_open(&importfile,fileName.toLatin1(),0);

    /* parse and build index of complete log file and show progress */
    while (dlt_file_read_raw(&importfile,false,0)>=0)
    {
        // https://bugreports.qt-project.org/browse/QTBUG-26069
        outputfile.seek(outputfile.size());
        outputfile.write((char*)importfile.msg.headerbuffer,importfile.msg.headersize);
        outputfile.write((char*)importfile.msg.databuffer,importfile.msg.datasize);
        outputfile.flush();

    }

    dlt_file_free(&importfile,0);

    if(importfile.error_messages>0)
    {
        QMessageBox::warning(this, QString("DLT Stream import"),
                             QString("At least %1 corrupted messages during import found!").arg(importfile.error_messages));
    }

    reloadLogFile();

}

void MainWindow::on_action_menuFile_Import_DLT_Stream_with_Serial_Header_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Import DLT Stream with serial header"), workingDirectory.getDltDirectory(), tr("DLT Stream file (*.*)"));

    if(fileName.isEmpty())
        return;

    /* change DLT file working directory */
    workingDirectory.setDltDirectory(QFileInfo(fileName).absolutePath());

    if(!outputfile.isOpen())
        return;

    DltFile importfile;

    dlt_file_init(&importfile,0);

    /* open DLT stream file */
    dlt_file_open(&importfile,fileName.toLatin1(),0);

    /* parse and build index of complete log file and show progress */
    while (dlt_file_read_raw(&importfile,true,0)>=0)
    {
        // https://bugreports.qt-project.org/browse/QTBUG-26069
        outputfile.seek(outputfile.size());
        outputfile.write((char*)importfile.msg.headerbuffer,importfile.msg.headersize);
        outputfile.write((char*)importfile.msg.databuffer,importfile.msg.datasize);
        outputfile.flush();

    }

    dlt_file_free(&importfile,0);

    if(importfile.error_messages>0)
    {
        QMessageBox::warning(this, QString("Import DLT Stream with serial header"),
                             QString("%1 corrupted messages during import found!").arg(importfile.error_messages));
    }

    reloadLogFile();
}

void MainWindow::on_action_menuFile_Append_DLT_File_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Append DLT File"), workingDirectory.getDltDirectory(), tr("DLT File (*.dlt)"));

    if(fileName.isEmpty())
        return;

    /* change DLT file working directory */
    workingDirectory.setDltDirectory(QFileInfo(fileName).absolutePath());

    if(!outputfile.isOpen())
        return;

    DltFile importfile;

    dlt_file_init(&importfile,0);

    QProgressDialog progress("Append log file", "Cancel Loading", 0, 100, this);
    progress.setModal(true);
    int num = 0;

    /* open DLT log file with same filename as output file */
    if (dlt_file_open(&importfile,fileName.toLatin1() ,0)<0)
    {
        return;
    }

    /* get number of files in DLT log file */
    while (dlt_file_read(&importfile,0)>=0)
    {
        num++;
        if ( 0 == (num%1000))
            progress.setValue(importfile.file_position*100/importfile.file_length);
        if (progress.wasCanceled())
        {
            dlt_file_free(&importfile,0);
            return;
        }
    }

    /* read DLT messages and append to current output file */
    for(int pos = 0 ; pos<num ; pos++)
    {
        if ( 0 == (pos%1000))
            progress.setValue(pos*100/num);
        if (progress.wasCanceled())
        {
            dlt_file_free(&importfile,0);
            reloadLogFile();
            return;
        }
        dlt_file_message(&importfile,pos,0);
        outputfile.write((char*)importfile.msg.headerbuffer,importfile.msg.headersize);
        outputfile.write((char*)importfile.msg.databuffer,importfile.msg.datasize);
    }
    outputfile.flush();

    dlt_file_free(&importfile,0);

    /* reload log file */
    reloadLogFile();

}

void MainWindow::exportSelection(bool ascii = true,bool file = false)
{
    Q_UNUSED(ascii);
    Q_UNUSED(file);

    QModelIndexList list = ui->tableView->selectionModel()->selection().indexes();

    DltExporter exporter;
    exporter.exportMessages(&qfile,0,&pluginManager,DltExporter::FormatClipboard,DltExporter::SelectionSelected,&list);
}

void MainWindow::on_actionExport_triggered()
{
    /* export dialog */
    exporterDialog.exec();
    if(exporterDialog.result() != QDialog::Accepted)
        return;

    DltExporter::DltExportFormat exportFormat = exporterDialog.getFormat();
    DltExporter::DltExportSelection exportSelection = exporterDialog.getSelection();
    QModelIndexList list = ui->tableView->selectionModel()->selection().indexes();

    /* check plausibility */
    if(exportSelection == DltExporter::SelectionAll)
    {
        qDebug() << "DLT Export of all" << qfile.size() << "messages";
        if(qfile.size() <= 0)
        {
            QMessageBox::critical(this, QString("DLT Viewer"),
                                  QString("Nothing to export. Make sure you have a DLT file open."));
            return;
        }
    }
    else if(exportSelection == DltExporter::SelectionFiltered)
    {
        qDebug() << "DLT Export of filterd" << qfile.sizeFilter() << "messages";
        if(qfile.sizeFilter() <= 0)
        {
            QMessageBox::critical(this, QString("DLT Viewer"),
                                  QString("Nothing to export. Make sure you have a DLT file open and that not everything is filtered."));
            return;
        }
    }
    else if(exportSelection == DltExporter::SelectionSelected)
    {
        qDebug() << "DLT Export of selected" << list.count() << "messages";
        if(list.count() <= 0)
        {
            QMessageBox::critical(this, QString("DLT Viewer"),
                                  QString("No messages selected. Select something from the main view."));
            return;
        }
    }

    /* ask for filename */
    QFileDialog dialog(this);
    QStringList filters;

    if(exportFormat == DltExporter::FormatDlt)
    {
        filters << "DLT Files (*.dlt)" <<"All files (*.*)";
        dialog.setDefaultSuffix("dlt");
        dialog.setWindowTitle("Export to DLT file");
        qDebug() << "DLT Export to Dlt";
    }
    else if(exportFormat == DltExporter::FormatAscii)
    {
        filters << "Ascii Files (*.txt)" <<"All files (*.*)";
        dialog.setDefaultSuffix("txt");
        dialog.setWindowTitle("Export to Ascii file");
        qDebug() << "DLT Export to Ascii";
    }
    else if(exportFormat == DltExporter::FormatCsv)
    {
        filters << "CSV Files (*.csv)" <<"All files (*.*)";
        dialog.setDefaultSuffix("csv");
        dialog.setWindowTitle("Export to CSV file");
        qDebug() << "DLT Export to CSV";
    }

    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(workingDirectory.getExportDirectory());
    dialog.setNameFilters(filters);
    dialog.exec();
    if(dialog.result() != QFileDialog::Accepted ||
        dialog.selectedFiles().count() < 1)
    {
        return;
    }
    QString fileName = dialog.selectedFiles()[0];

    if(fileName.isEmpty())
        return;

    /* change last export directory */
    workingDirectory.setExportDirectory(QFileInfo(fileName).absolutePath());
    DltExporter exporter(this);
    QFile outfile(fileName);

    if(exportSelection == DltExporter::SelectionSelected)
        exporter.exportMessages(&qfile, &outfile, &pluginManager,exportFormat,exportSelection,&list);
    else
        exporter.exportMessages(&qfile, &outfile, &pluginManager,exportFormat,exportSelection);

}

void MainWindow::on_action_menuFile_SaveAs_triggered()
{

    QFileDialog dialog(this);
    QStringList filters;
    filters << "DLT Files (*.dlt)" <<"All files (*.*)";
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("dlt");
    dialog.setDirectory(workingDirectory.getDltDirectory());
    dialog.setNameFilters(filters);
    dialog.setWindowTitle("Save DLT Log file");
    dialog.exec();
    if(dialog.result() != QFileDialog::Accepted ||
        dialog.selectedFiles().count() < 1)
    {
        return;
    }

    QString fileName = dialog.selectedFiles()[0];

    if(fileName.isEmpty() || dialog.result() == QDialog::Rejected)
    {
        return;
    }

    on_SaveAs_triggered(fileName);
}

void MainWindow::on_SaveAs_triggered(QString fileName)
{

    /* check if filename is the same as already open */
    if(outputfile.fileName()==fileName)
    {
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("File is already open!"));
        return;
    }

    /* change DLT file working directory */
    workingDirectory.setDltDirectory(QFileInfo(fileName).absolutePath());

    qfile.close();
    outputfile.close();

    QFile sourceFile( outputfile.fileName() );
    QFile destFile( fileName );

    /* Dialog will ask if you want to replace */
    if(destFile.exists())
    {
        if(!destFile.remove())
        {
            QMessageBox::critical(0, QString("DLT Viewer"),
                                  QString("Save as failed! Could not delete old file."));
            return;
        }
    }


    if(!sourceFile.copy(destFile.fileName()))
    {
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("Save as failed! Could not move to new destination."));
        return;
    }

    outputfile.setFileName(fileName);
    outputfileIsTemporary = false;
    outputfileIsFromCLI = false;
    setCurrentFile(fileName);
    if(outputfile.open(QIODevice::WriteOnly|QIODevice::Append))
    {
        openFileNames = QStringList(fileName);
        reloadLogFile();
    }
    else
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("Cannot rename log file \"%1\"\n%2")
                              .arg(fileName)
                              .arg(outputfile.errorString()));
}

void MainWindow::on_action_menuFile_Clear_triggered()
{
    QString fn = DltFileUtils::createTempFile(DltFileUtils::getTempPath(settings));
    if(!fn.length())
    {
        /* Something went horribly wrong with file name creation
         * There's nothing we can do at this point */
        return;
    }

    QString oldfn = outputfile.fileName();

    if(outputfile.isOpen())
    {
        if (outputfile.size() == 0)
        {
            deleteactualFile();
        }
        else
        {
            outputfile.close();
        }
    }

    outputfile.setFileName(fn);

    if(outputfile.open(QIODevice::WriteOnly|QIODevice::Truncate))
    {
        openFileNames = QStringList(fn);
        reloadLogFile();
    }
    else
    {
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("Cannot open log file \"%1\"\n%2")
                              .arg(fn)
                              .arg(outputfile.errorString()));
    }

    if(outputfileIsTemporary && !settings->tempSaveOnClear && !outputfileIsFromCLI)
    {
        QFile dfile(oldfn);
        if(!dfile.remove())
        {
            QMessageBox::critical(0, QString("DLT Viewer"),
                                  QString("Cannot delete log file \"%1\"\n%2")
                                  .arg(oldfn)
                                  .arg(dfile.errorString()));
        }
    }
    outputfileIsTemporary = true;
    outputfileIsFromCLI = false;
    return;
}

void MainWindow::contextLoadingFile(QDltMsg &msg)
{
    /* analyse message, check if DLT control message response */
    if ( (msg.getType()==QDltMsg::DltTypeControl) && (msg.getSubtype()==QDltMsg::DltControlResponse))
    {
        /* find ecu item */
        EcuItem *ecuitemFound = 0;
        for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
        {
            EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
            if(ecuitem->id == msg.getEcuid())
            {
                ecuitemFound = ecuitem;
                break;
            }
        }

        if(!ecuitemFound)
        {
            /* no Ecuitem found, create a new one */
            ecuitemFound = new EcuItem(0);

            /* update ECU item */
            ecuitemFound->id = msg.getEcuid();
            ecuitemFound->update();

            /* add ECU to configuration */
            project.ecu->addTopLevelItem(ecuitemFound);

            /* Update the ECU list in control plugins */
            updatePluginsECUList();

            pluginManager.stateChanged(project.ecu->indexOfTopLevelItem(ecuitemFound), QDltConnection::QDltConnectionOffline);

        }

        controlMessage_ReceiveControlMessage(ecuitemFound,msg);
    }
}

void MainWindow::reloadLogFileStop()
{
}

void MainWindow::reloadLogFileProgressMax(quint64 num)
{
    statusProgressBar->setRange(0,num);
}

void MainWindow::reloadLogFileProgress(quint64 num)
{
    statusProgressBar->setValue(num);
}

void MainWindow::reloadLogFileProgressText(QString text)
{
    statusProgressBar->setFormat(QString("%1 %p%").arg(text));
}

void MainWindow::reloadLogFileVersionString(QString ecuId, QString version)
{
    // version message found in loading file
    if(!autoloadPluginsVersionEcus.contains(ecuId))
    {
        autoloadPluginsVersionStrings.append(version);
        autoloadPluginsVersionEcus.append(ecuId);

        statusFileVersion->setText("Version: "+autoloadPluginsVersionStrings.join(" "));

        if(settings->pluginsAutoloadPath)
        {
            pluginsAutoload(version);
        }
    }
}

void MainWindow::reloadLogFileFinishIndex()
{
    // show already unfiltered messages
    tableModel->setForceEmpty(false);
    tableModel->modelChanged();
    this->update(); // force update
    restoreSelection();

}

void MainWindow::reloadLogFileFinishFilter()
{
    // unlock table view
    //ui->tableView->unlock();

    // run through all viewer plugins
    // must be run in the UI thread, if some gui actions are performed
    if((dltIndexer->getMode() == DltFileIndexer::modeIndexAndFilter) && dltIndexer->getPluginsEnabled())
    {
        QList<QDltPlugin*> activeViewerPlugins;
        activeViewerPlugins = pluginManager.getViewerPlugins();
        for(int i = 0; i < activeViewerPlugins.size(); i++){
            QDltPlugin *item = (QDltPlugin*)activeViewerPlugins.at(i);
            item->initFileFinish();
        }
    }

    // enable filter if requested
    qfile.enableFilter(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool());
    qfile.enableSortByTime(DltSettingsManager::getInstance()->value("startup/sortByTimeEnabled", false).toBool());

    // updateIndex, if messages are received in between
    updateIndex();

    // update table
    tableModel->setForceEmpty(false);
    tableModel->modelChanged();
    this->update(); // force update
    restoreSelection();
    m_searchtableModel->modelChanged();

    // process getLogInfoMessages
    if(( dltIndexer->getMode() == DltFileIndexer::modeIndexAndFilter) && settings->updateContextLoadingFile)
    {
        QList<int> list = dltIndexer->getGetLogInfoList();
        QDltMsg msg;

        for(int num=0;num<list.size();num++)
        {
            if(qfile.getMsg(list[num],msg))
                contextLoadingFile(msg);
        }
    }

    // reconnect ecus again
    connectPreviouslyConnectedECUs();

    // We might have had readyRead events, which we missed
    readyRead();

    // hide progress bar when finished
    statusProgressBar->reset();
    statusProgressBar->hide();

}

void MainWindow::reloadLogFileFinishDefaultFilter()
{
    // hide progress bar when finished
    statusProgressBar->reset();
    statusProgressBar->hide();
}

void MainWindow::reloadLogFile(bool update, bool multithreaded)
{
    /* check if in logging only mode, then do not create index */
    tableModel->setLoggingOnlyMode(settings->loggingOnlyMode);
    tableModel->modelChanged();
    if(settings->loggingOnlyMode)
    {
        return;
    }

    /* clear autoload plugins ecu list */
    if(!update)
    {
        autoloadPluginsVersionEcus.clear();
        autoloadPluginsVersionStrings.clear();
        statusFileVersion->setText("Version: <unknown>");
    }

    // update indexFilter only if index already generated
    if(update)
    {   if(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool())
            dltIndexer->setMode(DltFileIndexer::modeFilter);
        else
            dltIndexer->setMode(DltFileIndexer::modeNone);
        saveSelection();
    }
    else
    {
        dltIndexer->setMode(DltFileIndexer::modeIndexAndFilter);
        clearSelection();
    }

    // prevent further receiving any new messages
    saveAndDisconnectCurrentlyConnectedSerialECUs();

    // clear all tables
    ui->tableView->selectionModel()->clear();
    m_searchtableModel->clear_SearchResults();
    ui->dockWidgetSearchIndex->hide();

    // force empty table
    tableModel->setForceEmpty(true);
    tableModel->modelChanged();

    // stop last indexing process, if any
    dltIndexer->stop();

    // open qfile
    if(!update)
    {
        for(int num=0;num<openFileNames.size();num++)
        {
            qDebug() << "Open file" << openFileNames[num];
            qfile.open(openFileNames[num],num!=0);
        }
    }
    //qfile.enableFilter(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool());
    qfile.enableFilter(false);

    // lock table view
    //ui->tableView->lock();

    // initialise progress bar
    statusProgressBar->reset();
    statusProgressBar->show();

    // set name of opened log file in status bar
    statusFilename->setText(outputfile.fileName());

    // enable plugins
    dltIndexer->setPluginsEnabled(DltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool());
    dltIndexer->setFiltersEnabled(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool());
    dltIndexer->setSortByTimeEnabled(DltSettingsManager::getInstance()->value("startup/sortByTimeEnabled", false).toBool());
    dltIndexer->setMultithreaded(multithreaded);
    if(settings->filterCache)
        dltIndexer->setFilterCache(settings->filterCacheName);
    else
        dltIndexer->setFilterCache(QString(""));

    // run through all viewer plugins
    // must be run in the UI thread, if some gui actions are performed
    if( (dltIndexer->getMode() == DltFileIndexer::modeIndexAndFilter) && dltIndexer->getPluginsEnabled())
    {
        QList<QDltPlugin*> activeViewerPlugins;
        activeViewerPlugins = pluginManager.getViewerPlugins();
        for(int i = 0; i < activeViewerPlugins.size(); i++){
            QDltPlugin *item = (QDltPlugin*)activeViewerPlugins.at(i);
            item->initFileStart(&qfile);
        }
    }

    // start indexing
    if(multithreaded)
        dltIndexer->start();
    else
        dltIndexer->run();

}

void MainWindow::reloadLogFileDefaultFilter()
{

    // stop last indexing process, if any
    dltIndexer->stop();

    // set indexing mode
    dltIndexer->setMode(DltFileIndexer::modeDefaultFilter);

    // initialise progress bar
    statusProgressBar->reset();
    statusProgressBar->show();

    // enable plugins
    dltIndexer->setPluginsEnabled(DltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool());
    dltIndexer->setFiltersEnabled(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool());
    dltIndexer->setSortByTimeEnabled(DltSettingsManager::getInstance()->value("startup/sortByTimeEnabled", false).toBool());

    // start indexing
    dltIndexer->start();
}

void MainWindow::applySettings()
{
    QFont tableViewFont = ui->tableView->font();
    tableViewFont.setPointSize(settings->fontSize);
    ui->tableView->setFont(tableViewFont);
    // Rescale the height of a row to choosen font size + 8 pixels
    ui->tableView->verticalHeader()->setDefaultSectionSize(settings->fontSize+8);

    settings->showIndex?ui->tableView->showColumn(0):ui->tableView->hideColumn(0);
    settings->showTime?ui->tableView->showColumn(1):ui->tableView->hideColumn(1);
    settings->showTimestamp?ui->tableView->showColumn(2):ui->tableView->hideColumn(2);
    settings->showCount?ui->tableView->showColumn(3):ui->tableView->hideColumn(3);

    settings->showEcuId?ui->tableView->showColumn(4):ui->tableView->hideColumn(4);
    settings->showApId?ui->tableView->showColumn(5):ui->tableView->hideColumn(5);
    settings->showCtId?ui->tableView->showColumn(6):ui->tableView->hideColumn(6);
    settings->showSessionId?ui->tableView->showColumn(7):ui->tableView->hideColumn(7);
    settings->showType?ui->tableView->showColumn(8):ui->tableView->hideColumn(8);

    settings->showSubtype?ui->tableView->showColumn(9):ui->tableView->hideColumn(9);
    settings->showMode?ui->tableView->showColumn(10):ui->tableView->hideColumn(10);
    settings->showNoar?ui->tableView->showColumn(11):ui->tableView->hideColumn(11);
    settings->showPayload?ui->tableView->showColumn(12):ui->tableView->hideColumn(12);

    DltSettingsManager *settingsmanager = DltSettingsManager::getInstance();

    int refreshRate = settingsmanager->value("RefreshRate",DEFAULT_REFRESH_RATE).toInt();
    if ( refreshRate )
        draw_interval = 1000 / refreshRate;
	else
		draw_interval = 1000 / DEFAULT_REFRESH_RATE;	
}

void MainWindow::on_action_menuFile_Settings_triggered()
{
    /* show settings dialog */
    settings->writeDlg();

    /* store old values */
    int defaultFilterPath = settings->defaultFilterPath;
    QString defaultFilterPathName = settings->defaultFilterPathName;
    int loggingOnlyMode=settings->loggingOnlyMode;

    if(settings->exec()==1)
    {
        /* change settings and store settings persistently */
        settings->readDlg();
        settings->writeSettings(this);

        /* Apply settings to table */
        applySettings();

        /* reload multifilter list if changed */
        if((defaultFilterPath != settings->defaultFilterPath)||(settings->defaultFilterPath && defaultFilterPathName != settings->defaultFilterPathName))
        {
            on_actionDefault_Filter_Reload_triggered();
        }

        updateScrollButton();

        if(loggingOnlyMode!=settings->loggingOnlyMode)
        {
            tableModel->setLoggingOnlyMode(settings->loggingOnlyMode);
            tableModel->modelChanged();
            if(!settings->loggingOnlyMode)
                QMessageBox::information(0, QString("DLT Viewer"),
                                         QString("Logging only mode disabled! Please reload DLT file to view file!"));


        }
    }
}

void MainWindow::on_action_menuFile_Quit_triggered()
{
    /* TODO: Add quit code here */
    this->close();

}


void MainWindow::on_action_menuProject_New_triggered()
{
    /* TODO: Ask for saving project if changed */

    /* create new project */

    this->setWindowTitle(QString("DLT Viewer - unnamed project - Version : %1 %2").arg(PACKAGE_VERSION).arg(PACKAGE_VERSION_STATE));
    project.Clear();

    /* Update the ECU list in control plugins */
    updatePluginsECUList();

}

void MainWindow::on_action_menuProject_Open_triggered()
{
    /* TODO: Ask for saving project if changed */

    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Open DLT Project file"), workingDirectory.getDlpDirectory(), tr("DLT Project Files (*.dlp);;All files (*.*)"));

    /* open existing project */
    if(!fileName.isEmpty())
    {

        openDlpFile(fileName);
    }

}

bool MainWindow::anyPluginsEnabled()
{
    if(!(DltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool()))
    {
        return false;
    }

    return (pluginManager.sizeEnabled()>0);
}

bool MainWindow::anyFiltersEnabled()
{
    if(!(DltSettingsManager::getInstance()->value("startup/filtersEnabled", true).toBool()))
    {
        return false;
    }
    bool foundEnabledFilter = false;
    for(int num = 0; num < project.filter->topLevelItemCount (); num++)
    {
        FilterItem *item = (FilterItem*)project.filter->topLevelItem(num);
        if(item->checkState(0) == Qt::Checked)
        {
            foundEnabledFilter = true;
            break;
        }
    }
    return foundEnabledFilter;
}

bool MainWindow::openDlfFile(QString fileName,bool replace)
{
    if(!fileName.isEmpty() && project.LoadFilter(fileName,replace))
    {
        workingDirectory.setDlfDirectory(QFileInfo(fileName).absolutePath());
        setCurrentFilters(fileName);
        applyConfigEnabled(true);
        on_filterWidget_itemSelectionChanged();
        ui->tabWidget->setCurrentWidget(ui->tabPFilter);
    }
    return true;
}

bool MainWindow::openDlpFile(QString fileName)
{
    /* Open existing project */
    if(project.Load(fileName))
    {
        /* Applies project settings and save it to registry */
        applySettings();
        settings->writeSettings(this);

        /* change Project file working directory */
        workingDirectory.setDlpDirectory(QFileInfo(fileName).absolutePath());

        this->setWindowTitle(QString("DLT Viewer - "+fileName+" - Version : %1 %2").arg(PACKAGE_VERSION).arg(PACKAGE_VERSION_STATE));

        /* Load the plugins description files after loading project */
        updatePlugins();

        setCurrentProject(fileName);

        /* Update the ECU list in control plugins */
        updatePluginsECUList();

        /* After loading the project file update the filters */
        filterUpdate();

        /* Finally, enable the 'Apply' button, if needed */
        if(anyPluginsEnabled() || anyFiltersEnabled())
        {
            applyConfigEnabled(true);
        }
        return true;
    } else {
        return false;
    }
}

void MainWindow::on_action_menuProject_Save_triggered()
{

    QFileDialog dialog(this);
    QStringList filters;
    filters << "DLT Project Files (*.dlp)" <<"All files (*.*)";
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("dlp");
    dialog.setDirectory(workingDirectory.getDlpDirectory());
    dialog.setNameFilters(filters);
    dialog.setWindowTitle("Save DLT Project file");
    dialog.exec();
    if(dialog.result() != QFileDialog::Accepted ||
        dialog.selectedFiles().count() < 1)
    {
        return;
    }




    QString fileName = dialog.selectedFiles()[0];


    /* save project */
    if(fileName.isEmpty() || dialog.result() == QDialog::Rejected)
    {
        //return;
    }
    else if( project.Save(fileName))
    {
        /* change Project file working directory */
        workingDirectory.setDlpDirectory(QFileInfo(fileName).absolutePath());

        this->setWindowTitle(QString("DLT Viewer - "+fileName+" - Version : %1 %2").arg(PACKAGE_VERSION).arg(PACKAGE_VERSION_STATE));

        setCurrentProject(fileName);
    }
}

QStringList MainWindow::getSerialPortsWithQextEnumerator(){

    QList<QextPortInfo> ports = QextSerialEnumerator::getPorts();
    QStringList portList;
#ifdef Q_OS_WIN
    for (int i = 0; i < ports.size(); i++) {
        portList << ports.at(i).portName;
    }
#else
    for (int i = 0; i < ports.size(); i++) {
        portList << ports.at(i).physName;
    }
#endif
    return portList;
}

void MainWindow::on_action_menuConfig_ECU_Add_triggered()
{   
    QStringList hostnameListPreset;
    hostnameListPreset << "localhost";

    QStringList portListPreset = getSerialPortsWithQextEnumerator();

    /* show ECU configuration dialog */
    EcuDialog dlg;
    EcuItem initItem;
    dlg.setData(initItem);

    /* Read settings for recent hostnames and ports */
    recentHostnames = DltSettingsManager::getInstance()->value("other/recentHostnameList",hostnameListPreset).toStringList();
    recentPorts = DltSettingsManager::getInstance()->value("other/recentPortList",portListPreset).toStringList();

    dlg.setHostnameList(recentHostnames);
    dlg.setPortList(recentPorts);

    if(dlg.exec()==1)
    {
        /* add new ECU to configuration */
        EcuItem* ecuitem = new EcuItem(0);
        dlg.setDialogToEcuItem(ecuitem);

        /* update ECU item */
        ecuitem->update();

        /* add ECU to configuration */
        project.ecu->addTopLevelItem(ecuitem);

        /* Update settings for recent hostnames and ports */
        setCurrentHostname(ecuitem->getHostname());
        setCurrentPort(ecuitem->getPort());

        /* Update the ECU list in control plugins */
        updatePluginsECUList();

        pluginManager.stateChanged(project.ecu->indexOfTopLevelItem(ecuitem), QDltConnection::QDltConnectionOffline);
    }
}

void MainWindow::on_action_menuConfig_ECU_Edit_triggered()
{
    /* find selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        QStringList hostnameListPreset;
        hostnameListPreset << "localhost";

        QStringList portListPreset = getSerialPortsWithQextEnumerator();

        EcuItem* ecuitem = (EcuItem*) list.at(0);

        /* show ECU configuration dialog */
        EcuDialog dlg;
        dlg.setData(*ecuitem);

        /* Read settings for recent hostnames and ports */
        recentHostnames = DltSettingsManager::getInstance()->value("other/recentHostnameList",hostnameListPreset).toStringList();
        recentPorts = DltSettingsManager::getInstance()->value("other/recentPortList",portListPreset).toStringList();

        setCurrentHostname(ecuitem->getHostname());

        //serial Port
        setCurrentPort(ecuitem->getPort());

        dlg.setHostnameList(recentHostnames);
        dlg.setPortList(recentPorts);

        if(dlg.exec())
        {
            bool interfaceChanged = false;
            if((ecuitem->interfacetype != dlg.interfacetype() ||
                ecuitem->getHostname() != dlg.hostname() ||
                ecuitem->getTcpport() != dlg.tcpport() ||
                ecuitem->getPort() != dlg.port() ||
                ecuitem->getBaudrate() != dlg.baudrate()) &&
                    ecuitem->tryToConnect)
            {
                interfaceChanged = true;
                disconnectECU(ecuitem);
            }

            dlg.setDialogToEcuItem(ecuitem);

            /* update ECU item */
            ecuitem->update();

            /* if interface settings changed, reconnect */
            if(interfaceChanged)
            {
                connectECU(ecuitem);
            }

            /* send new default log level to ECU, if connected and if selected in dlg */
            if(ecuitem->connected && ecuitem->updateDataIfOnline)
            {
                sendUpdates(ecuitem);
            }

            /* Update settings for recent hostnames and ports */
            setCurrentHostname(ecuitem->getHostname());
            setCurrentPort(ecuitem->getPort());

            /* Update the ECU list in control plugins */
            updatePluginsECUList();

        }
    }
}

void MainWindow::on_action_menuConfig_ECU_Delete_triggered()
{
    /* find selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        /* disconnect, if connected */
        disconnectECU((EcuItem*)list.at(0));

        /* delete ECU from configuration */
        delete project.ecu->takeTopLevelItem(project.ecu->indexOfTopLevelItem(list.at(0)));

        /* Update the ECU list in control plugins */
        updatePluginsECUList();
    }
}

void MainWindow::on_action_menuConfig_Delete_All_Contexts_triggered()
{
    /* find selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        /* delete all applications from ECU from configuration */
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        (ecuitem->takeChildren()).clear();
    }
}

void MainWindow::on_action_menuConfig_Application_Add_triggered()
{
    /* find selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        /* show Application configuration dialog */
        ApplicationDialog dlg("APP","A new Application");
        EcuItem* ecuitem = (EcuItem*)list.at(0);
        if(dlg.exec()==1)
        {
            /* change settings of application configuration */
            ApplicationItem* appitem = new ApplicationItem(ecuitem);
            appitem->id = dlg.id();
            appitem->description = dlg.description();

            /* update application item */
            appitem->update();

            /* add new application to ECU */
            ecuitem->addChild(appitem);
        }
    }
}

void MainWindow::on_action_menuConfig_Application_Edit_triggered()
{
    /* find selected application in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == application_type))
    {
        ApplicationItem* appitem = (ApplicationItem*) list.at(0);

        /* show Application configuration dialog */
        ApplicationDialog dlg(appitem->id,appitem->description);
        if(dlg.exec())
        {
            appitem->id = dlg.id();
            appitem->description = dlg.description();

            /* update application item */
            appitem->update();
        }
    }

}

void MainWindow::on_action_menuConfig_Application_Delete_triggered()
{
    /* find selected application in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == application_type))
    {
        ApplicationItem* appitem = (ApplicationItem*) list.at(0);

        /* remove application */
        delete appitem->parent()->takeChild(appitem->parent()->indexOfChild(appitem));
    }

}

void MainWindow::on_action_menuConfig_Context_Add_triggered()
{
    /* find selected application in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == application_type))
    {

        /* show Context configuration dialog */
        ContextDialog dlg("CON","A new Context",-1,-1);
        ApplicationItem* appitem = (ApplicationItem*)list.at(0);
        if(dlg.exec()==1)
        {
            ContextItem* conitem = new ContextItem(appitem);
            conitem->id = dlg.id();
            conitem->description = dlg.description();
            conitem->loglevel = dlg.loglevel();
            conitem->tracestatus = dlg.tracestatus();

            /* update context item */
            conitem->update();

            /* add new context to application */
            appitem->addChild(conitem);

            /* send new default log level to ECU, if connected and if selected in dlg */
            if(dlg.update())
            {
                EcuItem* ecuitem = (EcuItem*) appitem->parent();
                controlMessage_SetLogLevel(ecuitem,appitem->id,conitem->id,conitem->loglevel);
                controlMessage_SetTraceStatus(ecuitem,appitem->id,conitem->id,conitem->tracestatus);

                /* update status */
                conitem->status = ContextItem::valid;
                conitem->update();
            }
        }
    }


}

void MainWindow::on_action_menuConfig_Context_Edit_triggered()
{
    /* find selected context in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == context_type))
    {
        ContextItem* conitem = (ContextItem*) list.at(0);

        /* show Context configuration dialog */
        ContextDialog dlg(conitem->id,conitem->description,conitem->loglevel,conitem->tracestatus);
        if(dlg.exec())
        {
            conitem->id = dlg.id();
            conitem->description = dlg.description();
            conitem->loglevel = dlg.loglevel();
            conitem->tracestatus = dlg.tracestatus();

            /* update context item */
            conitem->update();

            /* send new log level to ECU, if connected and if selected in dlg */
            if(dlg.update())
            {
                ApplicationItem* appitem = (ApplicationItem*) conitem->parent();
                EcuItem* ecuitem = (EcuItem*) appitem->parent();

                if(ecuitem->connected)
                {
                    controlMessage_SetLogLevel(ecuitem,appitem->id,conitem->id,conitem->loglevel);
                    controlMessage_SetTraceStatus(ecuitem,appitem->id,conitem->id,conitem->tracestatus);

                    /* update status */
                    conitem->status = ContextItem::valid;
                    conitem->update();
                }
            }
        }
    }
}

void MainWindow::on_action_menuDLT_Edit_All_Log_Levels_triggered()
{

    MultipleContextDialog dlg(0,0);

    if(dlg.exec())
    {

        QList<QTreeWidgetItem *> list = project.ecu->selectedItems();

        if(list.at(0)->type() == context_type){
            //Nothing to do
        }

        if(list.at(0)->type() == application_type){
            //qDebug()<<"Application selected";
            ApplicationItem *applicationItem;
            for(int i=0; i<list.count(); i++){
                applicationItem = (ApplicationItem*) list.at(i);
                ContextItem *contextItem;
                for(int j=0; j<applicationItem->childCount();j++){
                    contextItem = (ContextItem*)applicationItem->child(j);
                    contextItem->setSelected(true);
                }
                applicationItem->setSelected(false);
            }
        }


        if(list.at(0)->type() == ecu_type){
            //qDebug()<<"ECU selected";
            EcuItem *ecuItem;
            for(int i=0; i<list.count(); i++){
                ecuItem = (EcuItem*) list.at(i);
                ApplicationItem *applicationItem;
                for(int j=0; j<ecuItem->childCount(); j++){
                    applicationItem = (ApplicationItem*) ecuItem->child(j);
                    ContextItem *contextItem;
                    for(int k=0; k<applicationItem->childCount();k++){
                        contextItem = (ContextItem*)applicationItem->child(k);
                        contextItem->setSelected(true);
                    }

                }
                ecuItem->setSelected(false);
            }
        }

        list = project.ecu->selectedItems();

        if((list.count() >= 1))
        {
            ContextItem *conitem;
            for(int i=0; i<list.count(); i++){
                if(list.at(i)->type() == context_type){

                    conitem = (ContextItem*) list.at(i);

                    conitem->loglevel = dlg.loglevel();
                    conitem->tracestatus = dlg.tracestatus();

                    /* update context item */
                    conitem->update();

                    /* send new log level to ECU, if connected and if selected in dlg */
                    if(dlg.update())
                    {
                        ApplicationItem* appitem = (ApplicationItem*) conitem->parent();
                        EcuItem* ecuitem = (EcuItem*) appitem->parent();

                        if(ecuitem->connected)
                        {
                            controlMessage_SetLogLevel(ecuitem,appitem->id,conitem->id,conitem->loglevel);
                            controlMessage_SetTraceStatus(ecuitem,appitem->id,conitem->id,conitem->tracestatus);

                            /* update status */
                            conitem->status = ContextItem::valid;
                            conitem->update();
                        }
                    }
                    conitem->setSelected(false);
                }
            }

        }
    }
}

void MainWindow::on_action_menuConfig_Context_Delete_triggered()
{
    /* find selected context in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == context_type))
    {
        ContextItem* conitem = (ContextItem*) list.at(0);

        /* delete context from application */
        delete conitem->parent()->takeChild(conitem->parent()->indexOfChild(conitem));
    }

}


void MainWindow::on_configWidget_customContextMenuRequested(QPoint pos)
{

    /* show custom pop menu  for configuration */
    QPoint globalPos = ui->configWidget->mapToGlobal(pos);
    QMenu menu(project.ecu);
    QAction *action;
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();

    if(list.count() > 1 && (list.at(0)->type() == context_type))
    {
        action = new QAction("&Edit All Log Levels...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Edit_All_Log_Levels_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("DLT &Set Log Levels...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Set_Log_Level_triggered()));
        menu.addAction(action);
    }
    else if((list.count() > 1) && (list.at(0)->type() == ecu_type))
    {
        action = new QAction("&Edit All Log Levels...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Edit_All_Log_Levels_triggered()));
        menu.addAction(action);
    }
    else if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        /* ECU is selected */

        action = new QAction("ECU Add...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_ECU_Add_triggered()));
        menu.addAction(action);

        action = new QAction("ECU Edit...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_ECU_Edit_triggered()));
        menu.addAction(action);

        action = new QAction("ECU Delete", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_ECU_Delete_triggered()));
        menu.addAction(action);

        action = new QAction("&ECU Edit All Log Levels...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Edit_All_Log_Levels_triggered()));
        menu.addAction(action);

        action = new QAction("ECU Delete All Contexts", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Delete_All_Contexts_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("Application Add...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Application_Add_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("ECU Connect", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Connect_triggered()));
        menu.addAction(action);

        action = new QAction("ECU Disconnect", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Disconnect_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("Expand All ECUs", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Expand_All_ECUs_triggered()));
        menu.addAction(action);

        action = new QAction("Collapse All ECUs", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Collapse_All_ECUs_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("DLT Get Log Info", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Get_Log_Info_triggered()));
        menu.addAction(action);

        action = new QAction("DLT Set All Log Levels", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Set_All_Log_Levels_triggered()));
        menu.addAction(action);

        action = new QAction("DLT Get Default Log Level", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Get_Default_Log_Level_triggered()));
        menu.addAction(action);

        action = new QAction("DLT Set Default Log Level", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Set_Default_Log_Level_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("Store Config", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Store_Config_triggered()));
        menu.addAction(action);

        action = new QAction("Reset to Factory Default", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Reset_to_Factory_Default_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("Send Injection...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Send_Injection_triggered()));
        menu.addAction(action);

        action = new QAction("Get Software Version", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Get_Software_Version_triggered()));
        menu.addAction(action);

        action = new QAction("Get Local Time", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Get_Local_Time_2_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("&Filter Add", this);
        connect(action, SIGNAL(triggered()), this, SLOT(filterAdd()));
        menu.addAction(action);




    }
    else if((list.count() > 1) && (list.at(0)->type() == application_type))
    {
        action = new QAction("&Edit All Log Levels...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Edit_All_Log_Levels_triggered()));
        menu.addAction(action);
    }
    else if((list.count() == 1) && (list.at(0)->type() == application_type))
    {
        /* Application is selected */

        action = new QAction("&Application Edit...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Application_Edit_triggered()));
        menu.addAction(action);

        action = new QAction("A&pplication Delete...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Application_Delete_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("&Context Add...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Context_Add_triggered()));
        menu.addAction(action);

        action = new QAction("&Edit All Log Levels...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Edit_All_Log_Levels_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("&Filter Add", this);
        connect(action, SIGNAL(triggered()), this, SLOT(filterAdd()));
        menu.addAction(action);


    }
    else if((list.count() == 1) && (list.at(0)->type() == context_type))
    {
        /* Context is selected */

        action = new QAction("&Context Edit...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Context_Edit_triggered()));
        menu.addAction(action);

        action = new QAction("C&ontext Delete...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Context_Delete_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("DLT &Set Log Level...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Set_Log_Level_triggered()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("&Filter Add", this);
        connect(action, SIGNAL(triggered()), this, SLOT(filterAdd()));
        menu.addAction(action);

        menu.addSeparator();

        action = new QAction("Send Injection...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuDLT_Send_Injection_triggered()));
        menu.addAction(action);
    }
    else
    {
        /* nothing is selected */
        action = new QAction("ECU Add...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_ECU_Add_triggered()));
        menu.addAction(action);

    }

    /* show popup menu */
    menu.exec(globalPos);

}


void MainWindow::on_filterWidget_customContextMenuRequested(QPoint pos)
{

    /* show custom pop menu  for filter configuration */
    QPoint globalPos = ui->filterWidget->mapToGlobal(pos);
    QMenu menu(project.ecu);
    QAction *action;
    QList<QTreeWidgetItem *> list = project.filter->selectedItems();


    action = new QAction("Save Filter...", this);
    if(project.filter->topLevelItemCount() <= 0)
        action->setEnabled(false);
    else
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Save_As_triggered()));
    menu.addAction(action);

    action = new QAction("Load Filter...", this);
    connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Load_triggered()));
    menu.addAction(action);

    action = new QAction("Append Filter...", this);
    connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Append_Filters_triggered()));
    menu.addAction(action);

    menu.addSeparator();

    action = new QAction("Filter Add...", this);
    connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Add_triggered()));
    menu.addAction(action);

    action = new QAction("Filter Edit...", this);
    if(list.size() != 1)
        action->setEnabled(false);
    else
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Edit_triggered()));
    menu.addAction(action);

    action = new QAction("Filter Duplicate...", this);
    if(list.size() != 1)
        action->setEnabled(false);
    else
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Duplicate_triggered()));
    menu.addAction(action);

    action = new QAction("Filter Delete", this);
    if(list.size() != 1)
        action->setEnabled(false);
    else
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Delete_triggered()));
    menu.addAction(action);

    /* show popup menu */
    menu.exec(globalPos);

}

void MainWindow::on_pluginWidget_customContextMenuRequested(QPoint pos)
{
    /* show custom pop menu for plugin configuration */
    QPoint globalPos = ui->pluginWidget->mapToGlobal(pos);
    QMenu menu(project.ecu);
    QAction *action;
    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();

    if((list.count() == 1) ) {
        PluginItem* item = (PluginItem*) list.at(0);

        action = new QAction("Plugin Edit...", this);
        connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuPlugin_Edit_triggered()));
        menu.addAction(action);
        menu.addSeparator();

        if(item->getPlugin()->isViewer())
        {
            /* If a viewer plugin is disabled, or enabled but not shown,
             * add 'show' action. Else add 'hide' action */
            if(item->getPlugin()->getMode() != QDltPlugin::ModeShow)
            {
                action = new QAction("Plugin Show", this);
                connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuPlugin_Show_triggered()));
                menu.addAction(action);
            }
            else
            {
                action = new QAction("Plugin Hide", this);
                connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuPlugin_Hide_triggered()));
                menu.addAction(action);
            }
        }

        /* If the plugin is shown or enabled, present the 'disable' option.
         * Else, present the 'enable' option */
        if(item->getMode() != QDltPlugin::ModeDisable)
        {
            action = new QAction("Plugin Disable", this);
            connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuPlugin_Disable_triggered()));
            menu.addAction(action);
        }
        else
        {
            action = new QAction("Plugin Enable", this);
            connect(action, SIGNAL(triggered()), this, SLOT(action_menuPlugin_Enable_triggered()));
            menu.addAction(action);
        }
        /* show popup menu */
        menu.exec(globalPos);
    }
}

void MainWindow::saveAndDisconnectCurrentlyConnectedSerialECUs()
{
    m_previouslyConnectedSerialECUs.clear();
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if(ecuitem->connected && ecuitem->interfacetype == 1)
        {
            m_previouslyConnectedSerialECUs.append(num);
            disconnectECU(ecuitem);
        }
    }
}

void MainWindow::connectPreviouslyConnectedECUs()
{
    for(int i=0;i<m_previouslyConnectedSerialECUs.size();i++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(m_previouslyConnectedSerialECUs.at(i));
        connectECU(ecuitem);
    }
}

void MainWindow::connectAll()
{
    if(project.ecu->topLevelItemCount() == 0)
    {
        on_action_menuConfig_ECU_Add_triggered();
    }

    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        connectECU(ecuitem);
    }
}

void MainWindow::disconnectAll()
{
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        disconnectECU(ecuitem);
    }
}

void MainWindow::disconnectECU(EcuItem *ecuitem)
{
    if(ecuitem->tryToConnect == true)
    {
        /* disconnect from host */
        ecuitem->tryToConnect = false;
        ecuitem->connected = false;
        ecuitem->connectError.clear();
        ecuitem->update();
        on_configWidget_itemSelectionChanged();

        /* update conenction state */
        if(ecuitem->interfacetype == 0)
        {
            /* TCP */
            if (ecuitem->socket.state()!=QAbstractSocket::UnconnectedState)
                ecuitem->socket.disconnectFromHost();
        }
        else
        {
            /* Serial */
            ecuitem->m_serialport->close();
        }

        ecuitem->InvalidAll();
    }
}

void MainWindow::on_action_menuConfig_Connect_triggered()
{
    /* get selected ECU from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        /* connect to host */
        connectECU(ecuitem);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));

}

void MainWindow::on_action_menuConfig_Disconnect_triggered()
{
    /* get selected ECU from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);
        disconnectECU(ecuitem);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::connectECU(EcuItem* ecuitem,bool force)
{
    if(ecuitem->tryToConnect == false || force)
    {
        ecuitem->tryToConnect = true;
        ecuitem->connected = false;
        //ecuitem->connectError.clear();
        ecuitem->update();
        on_configWidget_itemSelectionChanged();

        /* reset receive buffer */
        ecuitem->totalBytesRcvd = 0;
        ecuitem->totalBytesRcvdLastTimeout = 0;
        ecuitem->tcpcon.clear();
        ecuitem->serialcon.clear();

        /* start socket connection to host */
        if(ecuitem->interfacetype == 0)
        {
            /* TCP */
            /* connect socket signals with window slots */
            if (ecuitem->socket.state()==QAbstractSocket::UnconnectedState)
            {
                disconnect(&ecuitem->socket,0,0,0);
                connect(&ecuitem->socket,SIGNAL(connected()),this,SLOT(connected()));
                connect(&ecuitem->socket,SIGNAL(disconnected()),this,SLOT(disconnected()));
                connect(&ecuitem->socket,SIGNAL(error(QAbstractSocket::SocketError)),this,SLOT(error(QAbstractSocket::SocketError)));
                connect(&ecuitem->socket,SIGNAL(readyRead()),this,SLOT(readyRead()));
                connect(&ecuitem->socket,SIGNAL(stateChanged(QAbstractSocket::SocketState)),this,SLOT(stateChangedTCP(QAbstractSocket::SocketState)));

                //disconnect(&ecuitem->socket,0,0,0);
                ecuitem->socket.connectToHost(ecuitem->getHostname(),ecuitem->getTcpport());
            }
        }
        else
        {
            /* Serial */
            if(!ecuitem->m_serialport)
            {
                PortSettings settings = {ecuitem->getBaudrate(), DATA_8, PAR_NONE, STOP_1, FLOW_OFF, 10}; //Before timeout was 1
                ecuitem->m_serialport = new QextSerialPort(ecuitem->getPort(),settings);
                connect(ecuitem->m_serialport, SIGNAL(readyRead()), this, SLOT(readyRead()));
                connect(ecuitem->m_serialport,SIGNAL(dsrChanged(bool)),this,SLOT(stateChangedSerial(bool)));
            }
            else{

                //to keep things consistent: delete old member, create new one
                //alternatively we could just close the port, and set the new settings.
                ecuitem->m_serialport->close();
                //delete(ecuitem->m_serialport);
                ecuitem->m_serialport->setBaudRate(ecuitem->getBaudrate());
                //ecuitem->m_serialport->setDataBits(settings.DataBits);
                //ecuitem->m_serialport->setFlowControl(settings.FlowControl);
                //ecuitem->m_serialport->setStopBits(settings.StopBits);
                //ecuitem->m_serialport->setParity(settings.Parity);
                //ecuitem->m_serialport->setTimeout(settings.Timeout_Millisec);
                ecuitem->m_serialport->setPortName(ecuitem->getPort());
            }

            if(ecuitem->m_serialport->isOpen())
            {
                ecuitem->m_serialport->close();
                ecuitem->m_serialport->setBaudRate(ecuitem->getBaudrate());
            }

            ecuitem->m_serialport->open(QIODevice::ReadWrite);

            if(ecuitem->m_serialport->isOpen())
            {
                ecuitem->connected = true;
                ecuitem->update();
                on_configWidget_itemSelectionChanged();

                /* send new default log level to ECU, if selected in dlg */
                if (ecuitem->updateDataIfOnline)
                {
                    sendUpdates(ecuitem);
                }

            }

        }

        if(  (settings->showCtId && settings->showCtIdDesc) || (settings->showApId && settings->showApIdDesc) ){
            controlMessage_GetLogInfo(ecuitem);
        }
    }
}

void MainWindow::connected()
{
    /* signal emited when connected to host */

    /* find socket which emited signal */
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if( &(ecuitem->socket) == sender())
        {
            /* update connection state */
            ecuitem->connected = true;
            ecuitem->connectError.clear();
            ecuitem->update();
            on_configWidget_itemSelectionChanged();

            /* reset receive buffer */
            ecuitem->totalBytesRcvd = 0;
            ecuitem->totalBytesRcvdLastTimeout = 0;
            ecuitem->tcpcon.clear();
            ecuitem->serialcon.clear();
        }
    }
}

void MainWindow::disconnected()
{
    /* signal emited when disconnected to host */

    /* find socket which emited signal */
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if( &(ecuitem->socket) == sender())
        {
            /* update connection state */
            ecuitem->connected = false;
            ecuitem->connectError.clear();
            ecuitem->InvalidAll();
            ecuitem->update();
            on_configWidget_itemSelectionChanged();

            /* disconnect socket signals from window slots */
            disconnect(&ecuitem->socket,0,0,0);
        }
    }
}


void MainWindow::timeout()
{
        for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
        {
            EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);

            /* Try to reconnect if the ecuitem has not received
             * new data for long enough time.
             * If the indexer is busy indexing,
             * do not disconnect yet. Wait for future timeouts until
             * indexer is free. */
            if(ecuitem->isAutoReconnectTimeoutPassed() &&
               dltIndexer->tryLock())
            {
                if(ecuitem->interfacetype == 0 && ecuitem->autoReconnect && ecuitem->connected == true && ecuitem->totalBytesRcvd == ecuitem->totalBytesRcvdLastTimeout)
                {
                    disconnectECU(ecuitem);
                    ecuitem->tryToConnect = true;
                }
                ecuitem->totalBytesRcvdLastTimeout = ecuitem->totalBytesRcvd;
                dltIndexer->unlock();
            }

            if( ecuitem->tryToConnect && !ecuitem->connected)
            {
                connectECU(ecuitem,true);
            }
        }
}

void MainWindow::error(QAbstractSocket::SocketError /* socketError */)
{
    /* signal emited when connection to host is not possible */

    /* find socket which emited signal */
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if( &(ecuitem->socket) == sender())
        {
            /* save error */
            ecuitem->connectError = ecuitem->socket.errorString();

            if(ecuitem->interfacetype == 0)
            {
                /* disconnect socket */
                ecuitem->socket.disconnectFromHost();
            }

            /* update connection state */
            ecuitem->connected = false;
            ecuitem->update();

            on_configWidget_itemSelectionChanged();
        }

    }
}

void MainWindow::readyRead()
{
    /* signal emited when socket received data */

    /* Delay reading, if indexer is working on the dlt file */
    if(dltIndexer->tryLock())
    {
        /* find socket which emited signal */
        for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
        {
            EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
            if( (&(ecuitem->socket) == sender()) || (ecuitem->m_serialport == sender()))
            {
                read(ecuitem);
            }
        }
        dltIndexer->unlock();
    }
}

void MainWindow::read(EcuItem* ecuitem)
{
    int32_t bytesRcvd = 0;
    QDltMsg qmsg;

    if (!ecuitem)
        return;

    QByteArray data;
    if(ecuitem->interfacetype == 0)
    {
        /* TCP */
        // bytesRcvd = ecuitem->socket.bytesAvailable();
        data = ecuitem->socket.readAll();
        bytesRcvd = data.size();
        ecuitem->tcpcon.add(data);
     }
    else if(ecuitem->m_serialport)
    {
        /* serial */
        // bytesRcvd = ecuitem->m_serialport->bytesAvailable();
        data = ecuitem->m_serialport->readAll();
        bytesRcvd = data.size();
        ecuitem->serialcon.add(data);
    }

    /* reading data; new data is added to the current buffer */
    if (bytesRcvd>0)
    {

        ecuitem->totalBytesRcvd += bytesRcvd;

        while((ecuitem->interfacetype == 0 && ecuitem->tcpcon.parse(qmsg)) ||
              (ecuitem->interfacetype == 1 && ecuitem->serialcon.parse(qmsg)))
        {
            struct timeval tv;

            DltStorageHeader str;
            str.pattern[0]='D';
            str.pattern[1]='L';
            str.pattern[2]='T';
            str.pattern[3]=0x01;

            /* get time of day */
            #if defined(_MSC_VER)
                time(&(storageheader->seconds));
            #else
                gettimeofday(&tv, NULL);
            #endif

            QDateTime time = QDateTime::currentDateTime();
            str.seconds = (time_t)tv.tv_sec; /* value is long */
            str.microseconds = (int32_t)tv.tv_usec; /* value is long */
            str.ecu[0]=0;
            str.ecu[1]=0;
            str.ecu[2]=0;
            str.ecu[3]=0;
            /* prepare storage header */
            if (!qmsg.getEcuid().isEmpty())
               dlt_set_id(str.ecu,qmsg.getEcuid().toLatin1());
            else
                dlt_set_id(str.ecu,ecuitem->id.toLatin1());

            /* check if message is matching the filter */
            if (outputfile.isOpen())
            {

                if ((settings->writeControl && (qmsg.getType()==QDltMsg::DltTypeControl)) || (!(qmsg.getType()==QDltMsg::DltTypeControl)))
                {
                    // https://bugreports.qt-project.org/browse/QTBUG-26069
                    outputfile.seek(outputfile.size());
                    QByteArray bufferHeader = qmsg.getHeader();
                    QByteArray bufferPayload = qmsg.getPayload();

                    // set start time when writing first data
                    if(startLoggingDateTime.isNull())
                        startLoggingDateTime = QDateTime::currentDateTime();

                    // check if files size limit reached
                    if(settings->maxFileSizeMB && ((outputfile.size()+sizeof(DltStorageHeader)+bufferHeader.size()+bufferPayload.size())>(((size_t)settings->maxFileSizeMB)*1000*1000)))
                    {
                        // get new filename
                        QFileInfo info(outputfile.fileName());
                        QString newFilename = info.baseName()+
                                (startLoggingDateTime.toString("__yyyyMMdd_hhmmss"))+
                                (QDateTime::currentDateTime().toString("__yyyyMMdd_hhmmss"))+
                                QString(".dlt");
                        QFileInfo infoNew(info.absolutePath(),newFilename);

                        // rename old file
                        outputfile.rename(infoNew.absoluteFilePath());

                        // set new start time
                        startLoggingDateTime = QDateTime::currentDateTime();

                        // create new file
                        on_New_triggered(info.absoluteFilePath());
                    }

                    // write datat into file
                    outputfile.write((char*)&str,sizeof(DltStorageHeader));
                    outputfile.write(bufferHeader);
                    outputfile.write(bufferPayload);

                    outputfile.flush();

                    /* in Logging only mode send all message to plugins */
                    if(settings->loggingOnlyMode)
                    {
                        QList<QDltPlugin*> activeViewerPlugins;
                        activeViewerPlugins = pluginManager.getViewerPlugins();
                        for(int i = 0; i < activeViewerPlugins.size(); i++){
                            QDltPlugin *item = (QDltPlugin*)activeViewerPlugins.at(i);
                            item->updateMsg(-1,qmsg);
                            pluginManager.decodeMsg(qmsg,!OptManager::getInstance()->issilentMode());
                            item->updateMsgDecoded(-1,qmsg);
                        }
                    }
                }
            }

            /* analyse received message, check if DLT control message response */
            if ( (qmsg.getType()==QDltMsg::DltTypeControl) && (qmsg.getSubtype()==QDltMsg::DltControlResponse))
            {
                controlMessage_ReceiveControlMessage(ecuitem,qmsg);
            }
        }

        if(ecuitem->interfacetype == 0)
        {
            /* TCP */
            totalByteErrorsRcvd+=ecuitem->tcpcon.bytesError;
            ecuitem->tcpcon.bytesError = 0;
            totalBytesRcvd+=ecuitem->tcpcon.bytesReceived;
            ecuitem->tcpcon.bytesReceived = 0;
            totalSyncFoundRcvd+=ecuitem->tcpcon.syncFound;
            ecuitem->tcpcon.syncFound = 0;
         }
        else if(ecuitem->m_serialport)
        {
            /* serial */
            totalByteErrorsRcvd+=ecuitem->serialcon.bytesError;
            ecuitem->serialcon.bytesError = 0;
            totalBytesRcvd+=ecuitem->serialcon.bytesReceived;
            ecuitem->serialcon.bytesReceived = 0;
            totalSyncFoundRcvd+=ecuitem->serialcon.syncFound;
            ecuitem->serialcon.syncFound = 0;
        }

        if (outputfile.isOpen() )
        {
            if(!dltIndexer->isRunning())
                updateIndex();
        }
    }
}

void MainWindow::updateIndex()
{
    QList<QDltPlugin*> activeViewerPlugins;
    QList<QDltPlugin*> activeDecoderPlugins;
    QDltPlugin *item = 0;
    QDltMsg qmsg;

    activeDecoderPlugins = pluginManager.getDecoderPlugins();
    activeViewerPlugins = pluginManager.getViewerPlugins();

    /* read received messages in DLT file parser and update DLT message list view */
    /* update indexes  and table view */
    int oldsize = qfile.size();
    qfile.updateIndex();

    bool silentMode = !OptManager::getInstance()->issilentMode();

    if(oldsize!=qfile.size())
    {
        // only run through viewer plugins, if new messages are added
        for(int i = 0; i < activeViewerPlugins.size(); i++)
        {
            item = activeViewerPlugins[i];
            item->updateFileStart();
        }
    }

    for(int num=oldsize;num<qfile.size();num++) {
        qmsg.setMsg(qfile.getMsg(num));

        for(int i = 0; i < activeViewerPlugins.size(); i++){
            item = activeViewerPlugins.at(i);
            item->updateMsg(num,qmsg);
        }

        pluginManager.decodeMsg(qmsg,silentMode);

        if(qfile.checkFilter(qmsg)) {
            qfile.addFilterIndex(num);
        }

        for(int i = 0; i < activeViewerPlugins.size(); i++){
            item = activeViewerPlugins[i];
            item->updateMsgDecoded(num,qmsg);
        }
    }

    if (!draw_timer.isActive())
        draw_timer.start(draw_interval);

    if(oldsize!=qfile.size())
    {
        // only run through viewer plugins, if new messages are added
        for(int i = 0; i < activeViewerPlugins.size(); i++){
            item = activeViewerPlugins.at(i);
            item->updateFileFinish();
        }
    }

}

void MainWindow::draw_timeout()
{
    drawUpdatedView();
}


void MainWindow::drawUpdatedView()
{

    statusByteErrorsReceived->setText(QString("Recv Errors: %1").arg(totalByteErrorsRcvd));
    statusBytesReceived->setText(QString("Recv: %1").arg(totalBytesRcvd));
    statusSyncFoundReceived->setText(QString("Sync found: %1").arg(totalSyncFoundRcvd));

    tableModel->modelChanged();

    //Line below would resize the payload column automatically so that the whole content is readable
    //ui->tableView->resizeColumnToContents(11); //Column 11 is the payload column
    if(settings->autoScroll) {
        ui->tableView->scrollToBottom();
    }

}

void MainWindow::on_tableView_selectionChanged(const QItemSelection & selected, const QItemSelection & deselected)
{
    Q_UNUSED(deselected);

    if(selected.size()>0)
    {
        QModelIndex index =  selected[0].topLeft();
        QDltPlugin *item = 0;
        QList<QDltPlugin*> activeViewerPlugins;
        QList<QDltPlugin*> activeDecoderPlugins;
        QDltMsg msg;
        int msgIndex;

        msgIndex = qfile.getMsgFilterPos(index.row());
        msg.setMsg(qfile.getMsgFilter(index.row()));
        activeViewerPlugins = pluginManager.getViewerPlugins();
        activeDecoderPlugins = pluginManager.getDecoderPlugins();

        qDebug() << "Message at row" << index.row() << "at index" << msgIndex << "selected.";
        qDebug() << "Viewer plugins" << activeViewerPlugins.size() << "decoder plugins" << activeDecoderPlugins.size() ;

        if(activeViewerPlugins.isEmpty() && activeDecoderPlugins.isEmpty())
        {
            return;
        }

        // Update plugins
        for(int i = 0; i < activeViewerPlugins.size() ; i++)
        {
            item = (QDltPlugin*)activeViewerPlugins.at(i);
            item->selectedIdxMsg(msgIndex,msg);

        }

        pluginManager.decodeMsg(msg,!OptManager::getInstance()->issilentMode());

        for(int i = 0; i < activeViewerPlugins.size(); i++){
            item = (QDltPlugin*)activeViewerPlugins.at(i);
            item->selectedIdxMsgDecoded(msgIndex,msg);
        }
    }
}

void MainWindow::controlMessage_ReceiveControlMessage(EcuItem *ecuitem, QDltMsg &msg)
{
    const char *ptr;
    int32_t length;

    QByteArray payload = msg.getPayload();
    ptr = payload.constData();
    length = payload.size();

    /* control message was received */
    uint32_t service_id=0, service_id_tmp=0;
    DLT_MSG_READ_VALUE(service_id_tmp,ptr,length,uint32_t);
    service_id=DLT_ENDIAN_GET_32( ((msg.getEndianness()==QDltMsg::DltEndiannessBigEndian)?DLT_HTYP_MSBF:0), service_id_tmp);

    /* check if plugin autoload enabled and
     * it is a version message and
       version string not already parsed */
    if(service_id == 0x13 &&
       !autoloadPluginsVersionEcus.contains(msg.getEcuid()))
    {
        versionString(msg);
        autoloadPluginsVersionEcus.append(msg.getEcuid());
    }

    switch (service_id)
    {
    case DLT_SERVICE_ID_GET_LOG_INFO:
    {
        /* Only status 1,2,6,7,8 is supported yet! */

        uint8_t status=0;
        DLT_MSG_READ_VALUE(status,ptr,length,uint8_t); /* No endian conversion necessary */

        /* Support for status=8 */
        if (status==8)
        {
            ecuitem->InvalidAll();
        }

        /* Support for status=6 and status=7 */
        if ((status==6) || (status==7))
        {
            uint16_t count_app_ids=0,count_app_ids_tmp=0;
            DLT_MSG_READ_VALUE(count_app_ids_tmp,ptr,length,uint16_t);
            count_app_ids=DLT_ENDIAN_GET_16(((msg.getEndianness()==QDltMsg::DltEndiannessBigEndian)?DLT_HTYP_MSBF:0), count_app_ids_tmp);

            for (int32_t num=0;num<count_app_ids;num++)
            {
                char apid[DLT_ID_SIZE+1];
                apid[DLT_ID_SIZE] = 0;

                DLT_MSG_READ_ID(apid,ptr,length);

                uint16_t count_context_ids=0,count_context_ids_tmp=0;
                DLT_MSG_READ_VALUE(count_context_ids_tmp,ptr,length,uint16_t);
                count_context_ids=DLT_ENDIAN_GET_16(((msg.getEndianness()==QDltMsg::DltEndiannessBigEndian)?DLT_HTYP_MSBF:0), count_context_ids_tmp);

                for (int32_t num2=0;num2<count_context_ids;num2++)
                {
                    QString contextDescription;
                    char ctid[DLT_ID_SIZE+1];
                    ctid[DLT_ID_SIZE] = 0;

                    DLT_MSG_READ_ID(ctid,ptr,length);

                    int8_t log_level=0;
                    DLT_MSG_READ_VALUE(log_level,ptr,length,int8_t); /* No endian conversion necessary */

                    int8_t trace_status=0;
                    DLT_MSG_READ_VALUE(trace_status,ptr,length,int8_t); /* No endian conversion necessary */

                    if (status==7)
                    {
                        uint16_t context_description_length=0,context_description_length_tmp=0;
                        DLT_MSG_READ_VALUE(context_description_length_tmp,ptr,length,uint16_t);
                        context_description_length=DLT_ENDIAN_GET_16(((msg.getEndianness()==QDltMsg::DltEndiannessBigEndian)?DLT_HTYP_MSBF:0),context_description_length_tmp);

                        if (length<context_description_length)
                        {
                            length = -1;
                        }
                        else
                        {
                            contextDescription = QString(QByteArray((char*)ptr,context_description_length));
                            ptr+=context_description_length;
                            length-=context_description_length;
                        }
                    }

                    controlMessage_SetContext(ecuitem,QString(apid),QString(ctid),contextDescription,log_level,trace_status);
                }

                if (status==7)
                {
                    QString applicationDescription;
                    uint16_t application_description_length=0,application_description_length_tmp=0;
                    DLT_MSG_READ_VALUE(application_description_length_tmp,ptr,length,uint16_t);
                    application_description_length=DLT_ENDIAN_GET_16(((msg.getEndianness()==QDltMsg::DltEndiannessBigEndian)?DLT_HTYP_MSBF:0),application_description_length_tmp);
                    applicationDescription = QString(QByteArray((char*)ptr,application_description_length));
                    controlMessage_SetApplication(ecuitem,QString(apid),applicationDescription);
                    ptr+=application_description_length;
                }
            }
        }

        char com_interface[DLT_ID_SIZE];
        DLT_MSG_READ_ID(com_interface,ptr,length);

        if (length<0)
        {
            // wxMessageBox(_("Control Message corrupted!"),_("Receive Control Message"));
        }

        break;
    }
    case DLT_SERVICE_ID_GET_DEFAULT_LOG_LEVEL:
    {
        uint8_t status=0;
        DLT_MSG_READ_VALUE(status,ptr,length,uint8_t); /* No endian conversion necessary */

        uint8_t loglevel=0;
        DLT_MSG_READ_VALUE(loglevel,ptr,length,uint8_t); /* No endian conversion necessary */

        switch (status)
        {
        case 0: /* OK */
        {
            ecuitem->loglevel = loglevel;
            ecuitem->status = EcuItem::valid;
        }
            break;
        case 1: /* NOT_SUPPORTED */
        {
            ecuitem->status = EcuItem::unknown;
        }
            break;
        case 2: /* ERROR */
        {
            ecuitem->status = EcuItem::invalid;
        }
            break;
        }
        /* update status */
        ecuitem->update();

        break;
    }
    case DLT_SERVICE_ID_SET_LOG_LEVEL:
    {
        uint8_t status=0;
        DLT_MSG_READ_VALUE(status,ptr,length,uint8_t); /* No endian conversion necessary */

        switch (status)
        {
        case 0: /* OK */
        {
            //conitem->status = EcuItem::valid;
        }
            break;
        case 1: /* NOT_SUPPORTED */
        {
            //conitem->status = EcuItem::unknown;
        }
            break;
        case 2: /* ERROR */
        {
            //conitem->status = EcuItem::invalid;
        }
            break;
        }

        /* update status*/
        //conitem->update();

        break;
    }
    case DLT_SERVICE_ID_TIMEZONE:
    {
        if(payload.size() == sizeof(DltServiceTimezone))
        {
            DltServiceTimezone *service;
            service = (DltServiceTimezone*) payload.constData();

            if(msg.getEndianness() == QDltMsg::DltEndiannessLittleEndian)
                controlMessage_Timezone(service->timezone, service->isdst);
            else
                controlMessage_Timezone(DLT_SWAP_32(service->timezone), service->isdst);
        }
        break;
    }
    case DLT_SERVICE_ID_UNREGISTER_CONTEXT:
    {
        if(payload.size() == sizeof(DltServiceUnregisterContext))
        {
            DltServiceUnregisterContext *service;
            service = (DltServiceUnregisterContext*) payload.constData();

            controlMessage_UnregisterContext(msg.getEcuid(),QString(QByteArray(service->apid,4)),QString(QByteArray(service->ctid,4)));
        }
        break;
    }
    } // switch
}

void MainWindow::controlMessage_SendControlMessage(EcuItem* ecuitem,DltMessage &msg, QString appid, QString contid)
{
    QByteArray data;
    QDltMsg qmsg;

    /* prepare storage header */
    msg.storageheader = (DltStorageHeader*)msg.headerbuffer;
    dlt_set_storageheader(msg.storageheader,ecuitem->id.toLatin1());

    /* prepare standard header */
    msg.standardheader = (DltStandardHeader*)(msg.headerbuffer + sizeof(DltStorageHeader));
    msg.standardheader->htyp = DLT_HTYP_WEID | DLT_HTYP_WTMS | DLT_HTYP_UEH | DLT_HTYP_PROTOCOL_VERSION1 ;

#if (BYTE_ORDER==BIG_ENDIAN)
    msg.standardheader->htyp = (msg.standardheader->htyp | DLT_HTYP_MSBF);
#endif

    msg.standardheader->mcnt = 0;

    /* Set header extra parameters */
    dlt_set_id(msg.headerextra.ecu,ecuitem->id.toLatin1());
    msg.headerextra.tmsp = dlt_uptime();

    /* Copy header extra parameters to headerbuffer */
    dlt_message_set_extraparameters(&msg,0);

    /* prepare extended header */
    msg.extendedheader = (DltExtendedHeader*)(msg.headerbuffer + sizeof(DltStorageHeader) + sizeof(DltStandardHeader) + DLT_STANDARD_HEADER_EXTRA_SIZE(msg.standardheader->htyp) );
    msg.extendedheader->msin = DLT_MSIN_CONTROL_REQUEST;
    msg.extendedheader->noar = 1; /* number of arguments */
    if (appid.isEmpty())
    {
        dlt_set_id(msg.extendedheader->apid,"APP");       /* application id */
    }
    else
    {
        dlt_set_id(msg.extendedheader->apid, appid.toLatin1());
    }
    if (contid.isEmpty())
    {
        dlt_set_id(msg.extendedheader->ctid,"CON");       /* context id */
    }
    else
    {
        dlt_set_id(msg.extendedheader->ctid, contid.toLatin1());
    }

    /* prepare length information */
    msg.headersize = sizeof(DltStorageHeader) + sizeof(DltStandardHeader) + sizeof(DltExtendedHeader) + DLT_STANDARD_HEADER_EXTRA_SIZE(msg.standardheader->htyp);
    msg.standardheader->len = DLT_HTOBE_16(msg.headersize - sizeof(DltStorageHeader) + msg.datasize);

    /* send message to daemon */
    if (ecuitem->interfacetype == 0 && ecuitem->socket.isOpen())
    {
        /* Optional: Send serial header, if requested */
        if (ecuitem->getSendSerialHeaderTcp())
            ecuitem->socket.write((const char*)dltSerialHeader,sizeof(dltSerialHeader));

        /* Send data */
        ecuitem->socket.write((const char*)msg.headerbuffer+sizeof(DltStorageHeader),msg.headersize-sizeof(DltStorageHeader));
        ecuitem->socket.write((const char*)msg.databuffer,msg.datasize);
    }
    else if (ecuitem->interfacetype == 1 && ecuitem->m_serialport && ecuitem->m_serialport->isOpen())
    {
        /* Optional: Send serial header, if requested */
        if (ecuitem->getSendSerialHeaderSerial())
            ecuitem->m_serialport->write((const char*)dltSerialHeader,sizeof(dltSerialHeader));

        /* Send data */
        ecuitem->m_serialport->write((const char*)msg.headerbuffer+sizeof(DltStorageHeader),msg.headersize-sizeof(DltStorageHeader));
        ecuitem->m_serialport->write((const char*)msg.databuffer,msg.datasize);
    }
    else
    {
        /* ECU is not connected */
        return;
    }

    /* Skip the file handling, if indexer is working on the file */
    if(dltIndexer->tryLock())
    {
        /* store ctrl message in log file */
        if (outputfile.isOpen())
        {
            if (settings->writeControl)
            {
                // https://bugreports.qt-project.org/browse/QTBUG-26069
                outputfile.seek(outputfile.size());
                outputfile.write((const char*)msg.headerbuffer,msg.headersize);
                outputfile.write((const char*)msg.databuffer,msg.datasize);
                outputfile.flush();
            }
        }

        /* read received messages in DLT file parser and update DLT message list view */
        /* update indexes  and table view */
        if(!dltIndexer->isRunning())
            updateIndex();

        dltIndexer->unlock();
    }
}

void MainWindow::controlMessage_WriteControlMessage(DltMessage &msg, QString appid, QString contid)
{
    QByteArray data;
    QDltMsg qmsg;

    /* prepare storage header */
    msg.storageheader = (DltStorageHeader*)msg.headerbuffer;
    dlt_set_storageheader(msg.storageheader,"DLTV");

    /* prepare standard header */
    msg.standardheader = (DltStandardHeader*)(msg.headerbuffer + sizeof(DltStorageHeader));
    msg.standardheader->htyp = DLT_HTYP_WEID | DLT_HTYP_WTMS | DLT_HTYP_UEH | DLT_HTYP_PROTOCOL_VERSION1 ;

#if (BYTE_ORDER==BIG_ENDIAN)
    msg.standardheader->htyp = (msg.standardheader->htyp | DLT_HTYP_MSBF);
#endif

    msg.standardheader->mcnt = 0;

    /* Set header extra parameters */
    dlt_set_id(msg.headerextra.ecu,"DLTV");
    msg.headerextra.tmsp = dlt_uptime();

    /* Copy header extra parameters to headerbuffer */
    dlt_message_set_extraparameters(&msg,0);

    /* prepare extended header */
    msg.extendedheader = (DltExtendedHeader*)(msg.headerbuffer + sizeof(DltStorageHeader) + sizeof(DltStandardHeader) + DLT_STANDARD_HEADER_EXTRA_SIZE(msg.standardheader->htyp) );
    msg.extendedheader->msin = DLT_MSIN_CONTROL_RESPONSE;
    msg.extendedheader->noar = 1; /* number of arguments */
    if (appid.isEmpty())
    {
        dlt_set_id(msg.extendedheader->apid,"DLTV");       /* application id */
    }
    else
    {
        dlt_set_id(msg.extendedheader->apid, appid.toLatin1());
    }
    if (contid.isEmpty())
    {
        dlt_set_id(msg.extendedheader->ctid,"DLTV");       /* context id */
    }
    else
    {
        dlt_set_id(msg.extendedheader->ctid, contid.toLatin1());
    }

    /* prepare length information */
    msg.headersize = sizeof(DltStorageHeader) + sizeof(DltStandardHeader) + sizeof(DltExtendedHeader) + DLT_STANDARD_HEADER_EXTRA_SIZE(msg.standardheader->htyp);
    msg.standardheader->len = DLT_HTOBE_16(msg.headersize - sizeof(DltStorageHeader) + msg.datasize);

    /* Skip the file handling, if indexer is working on the file */
    if(dltIndexer->tryLock())
    {
        /* store ctrl message in log file */
        if (outputfile.isOpen())
        {
            if (settings->writeControl)
            {
                // https://bugreports.qt-project.org/browse/QTBUG-26069
                outputfile.seek(outputfile.size());
                outputfile.write((const char*)msg.headerbuffer,msg.headersize);
                outputfile.write((const char*)msg.databuffer,msg.datasize);
                outputfile.flush();
            }
        }

        /* read received messages in DLT file parser and update DLT message list view */
        /* update indexes  and table view */
        if(!dltIndexer->isRunning())
            updateIndex();

        dltIndexer->unlock();
    }
}

void MainWindow::on_action_menuDLT_Get_Default_Log_Level_triggered()
{
    /* get selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        /* send get default log level request */
        ControlServiceRequest(ecuitem,DLT_SERVICE_ID_GET_DEFAULT_LOG_LEVEL);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Set_Default_Log_Level_triggered()
{
    /* get selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        /* send set default log level request */
        controlMessage_SetDefaultLogLevel(ecuitem,ecuitem->loglevel);
        controlMessage_SetDefaultTraceStatus(ecuitem,ecuitem->tracestatus);

        /* update status */
        ecuitem->status = EcuItem::valid;
        ecuitem->update();
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Set_Log_Level_triggered()
{
    /* get selected context in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == context_type))
    {
        ContextItem* conitem = (ContextItem*) list.at(0);
        ApplicationItem* appitem = (ApplicationItem*) conitem->parent();
        EcuItem* ecuitem = (EcuItem*) appitem->parent();

        /* send set log level and trace status request */
        controlMessage_SetLogLevel(ecuitem,appitem->id,conitem->id,conitem->loglevel);
        controlMessage_SetTraceStatus(ecuitem,appitem->id,conitem->id,conitem->tracestatus);

        /* update status */
        conitem->status = ContextItem::valid;
        conitem->update();

    }else if( (list.count() > 1) && (list.at(0)->type() == context_type) ){
        ContextItem* conitem;

        for(int i=0; i<list.count(); i++){
            if(list.at(i)->type() == context_type){
                conitem = (ContextItem*) list.at(i);

                ApplicationItem* appitem = (ApplicationItem*) conitem->parent();
                EcuItem* ecuitem = (EcuItem*) appitem->parent();

                /* send set log level and trace status request */
                controlMessage_SetLogLevel(ecuitem,appitem->id,conitem->id,conitem->loglevel);
                controlMessage_SetTraceStatus(ecuitem,appitem->id,conitem->id,conitem->tracestatus);

                /* update status */
                conitem->status = ContextItem::valid;
                conitem->update();
            }
        }
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Context selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Set_All_Log_Levels_triggered()
{
    /* get selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        /* iterate through all applications */
        for(int numapp = 0; numapp < ecuitem->childCount(); numapp++)
        {
            ApplicationItem * appitem = (ApplicationItem *) ecuitem->child(numapp);

            /* iterate through all contexts */
            for(int numcontext = 0; numcontext < appitem->childCount(); numcontext++)
            {
                ContextItem * conitem = (ContextItem *) appitem->child(numcontext);

                /* set log level and trace status of this context */
                controlMessage_SetLogLevel(ecuitem,appitem->id,conitem->id,conitem->loglevel);
                controlMessage_SetTraceStatus(ecuitem,appitem->id,conitem->id,conitem->tracestatus);

                /* update status */
                conitem->status = ContextItem::valid;
                conitem->update();

            }
        }
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Get_Log_Info_triggered()
{
    /* get selected ECU in configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);
        controlMessage_GetLogInfo(ecuitem);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::controlMessage_SetLogLevel(EcuItem* ecuitem, QString app, QString con,int log_level)
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceSetLogLevel);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceSetLogLevel *req;
    req = (DltServiceSetLogLevel*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_SET_LOG_LEVEL;
    dlt_set_id(req->apid,app.toLatin1());
    dlt_set_id(req->ctid,con.toLatin1());
    req->log_level = log_level;
    dlt_set_id(req->com,"remo");

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::controlMessage_SetDefaultLogLevel(EcuItem* ecuitem, int status)
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceSetDefaultLogLevel);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceSetDefaultLogLevel *req;
    req = (DltServiceSetDefaultLogLevel*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_SET_DEFAULT_LOG_LEVEL;
    req->log_level = status;
    dlt_set_id(req->com,"remo");

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::controlMessage_SetTraceStatus(EcuItem* ecuitem,QString app, QString con,int status)
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceSetLogLevel);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceSetLogLevel *req;
    req = (DltServiceSetLogLevel*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_SET_TRACE_STATUS;
    dlt_set_id(req->apid,app.toLatin1());
    dlt_set_id(req->ctid,con.toLatin1());
    req->log_level = status;
    dlt_set_id(req->com,"remo");

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);

}

void MainWindow::controlMessage_SetDefaultTraceStatus(EcuItem* ecuitem, int status)
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceSetDefaultLogLevel);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceSetDefaultLogLevel *req;
    req = (DltServiceSetDefaultLogLevel*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_SET_DEFAULT_TRACE_STATUS;
    req->log_level = status;
    dlt_set_id(req->com,"remo");

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::controlMessage_SetVerboseMode(EcuItem* ecuitem, int verbosemode)
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceSetVerboseMode);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceSetVerboseMode *req;
    req = (DltServiceSetVerboseMode*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_SET_VERBOSE_MODE;
    req->new_status = verbosemode;
    //dlt_set_id(req->com,"remo");

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::controlMessage_SetTimingPackets(EcuItem* ecuitem, bool enable)
{
    DltMessage msg;
    uint8_t new_status=(enable?1:0);

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload of data */
    msg.datasize = sizeof(DltServiceSetVerboseMode);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceSetVerboseMode *req;
    req = (DltServiceSetVerboseMode*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_SET_TIMING_PACKETS;
    req->new_status = new_status;

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::controlMessage_GetLogInfo(EcuItem* ecuitem)
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceGetLogInfoRequest);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceGetLogInfoRequest *req;
    req = (DltServiceGetLogInfoRequest*) msg.databuffer;
    req->service_id = DLT_SERVICE_ID_GET_LOG_INFO;

    req->options = 7;

    dlt_set_id(req->apid, "");
    dlt_set_id(req->ctid, "");

    dlt_set_id(req->com,"remo");

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::ControlServiceRequest(EcuItem* ecuitem, int service_id )
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload of data */
    msg.datasize = sizeof(uint32_t);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    uint32_t sid = service_id;
    memcpy(msg.databuffer,&sid,sizeof(sid));

    /* send message */
    controlMessage_SendControlMessage(ecuitem,msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::controlMessage_Marker()
{
    DltMessage msg;

    /* initialise new message */
    dlt_message_init(&msg,0);

    /* prepare payload */
    msg.datasize = sizeof(DltServiceMarker);
    if (msg.databuffer) free(msg.databuffer);
    msg.databuffer = (uint8_t *) malloc(msg.datasize);
    DltServiceMarker *resp;
    resp = (DltServiceMarker*) msg.databuffer;
    resp->service_id = DLT_SERVICE_ID_MARKER;
    resp->status = DLT_SERVICE_RESPONSE_OK;

    /* send message */
    controlMessage_WriteControlMessage(msg,QString(""),QString(""));

    /* free message */
    dlt_message_free(&msg,0);
}

void MainWindow::SendInjection(EcuItem* ecuitem)
{
    unsigned int serviceID;
    unsigned int size;
    bool ok;

    if (injectionAplicationId.isEmpty() || injectionContextId.isEmpty() || injectionServiceId.isEmpty() )
        return;

    serviceID = (unsigned int)injectionServiceId.toInt(&ok, 0);

    if ((DLT_SERVICE_ID_CALLSW_CINJECTION<= serviceID) && (serviceID!=0))
    {
        DltMessage msg;
        QByteArray hexData;

        /* initialise new message */
        dlt_message_init(&msg,0);

        // Request parameter:
        // data_length uint32
        // data        uint8[]

        /* prepare payload of data */
        if(injectionDataBinary)
        {
            hexData = QByteArray::fromHex(injectionData.toLatin1());
            size = hexData.size();
        }
        else
        {
            size = (injectionData.length() + 1);
        }

        msg.datasize = 4 + 4 + size;
        if (msg.databuffer) free(msg.databuffer);
        msg.databuffer = (uint8_t *) malloc(msg.datasize);

        memcpy(msg.databuffer  , &serviceID,sizeof(serviceID));
        memcpy(msg.databuffer+4, &size, sizeof(size));

        if(injectionDataBinary)
        {
            memcpy(msg.databuffer+8,hexData.data(),hexData.size());
        }
        else
        {
            memcpy(msg.databuffer+8, injectionData.toUtf8(), size);
        }

        /* send message */
        controlMessage_SendControlMessage(ecuitem,msg,injectionAplicationId,injectionContextId);

        /* free message */
        dlt_message_free(&msg,0);
    }
}

void MainWindow::on_action_menuDLT_Store_Config_triggered()
{
    /* get selected ECU from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        ControlServiceRequest(ecuitem,DLT_SERVICE_ID_STORE_CONFIG);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Reset_to_Factory_Default_triggered()
{
    /* get selected ECU from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        ControlServiceRequest(ecuitem,DLT_SERVICE_ID_RESET_TO_FACTORY_DEFAULT);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Get_Software_Version_triggered()
{
    /* get selected ECU from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        ControlServiceRequest(ecuitem,DLT_SERVICE_ID_GET_SOFTWARE_VERSION);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}

void MainWindow::on_action_menuDLT_Get_Local_Time_2_triggered()
{
    /* get selected ECU from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        EcuItem* ecuitem = (EcuItem*) list.at(0);

        ControlServiceRequest(ecuitem,DLT_SERVICE_ID_GET_LOCAL_TIME);
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No ECU selected in configuration!"));
}


void MainWindow::getSelectedItems(EcuItem **ecuitem,ApplicationItem** appitem,ContextItem** conitem)
{
    *ecuitem = 0;
    *appitem = 0;
    *conitem = 0;

    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if(list.count() != 1)
    {
        return;
    }

    if(list.at(0)->type() == ecu_type)
    {
        *ecuitem = (EcuItem*) list.at(0);
    }
    if(list.at(0)->type() == application_type)
    {
        *appitem = (ApplicationItem*) list.at(0);
        *ecuitem = (EcuItem*) (*appitem)->parent();
    }
    if(list.at(0)->type() == context_type)
    {
        *conitem = (ContextItem*) list.at(0);
        *appitem = (ApplicationItem*) (*conitem)->parent();
        *ecuitem = (EcuItem*) (*appitem)->parent();
    }

}

void MainWindow::connectEcuSignal(int index)
{
    EcuItem* ecuitem = (EcuItem*) project.ecu->topLevelItem(index);

    if(ecuitem)
    {
        connectECU(ecuitem);
    }
}

void MainWindow:: disconnectEcuSignal(int index)
{
    EcuItem* ecuitem = (EcuItem*) project.ecu->topLevelItem(index);

    if(ecuitem)
    {
        disconnectECU(ecuitem);
    }
}


void MainWindow::sendInjection(int index,QString applicationId,QString contextId,int serviceId,QByteArray data)
{
    EcuItem* ecuitem = (EcuItem*) project.ecu->topLevelItem(index);

    injectionAplicationId = applicationId;
    injectionContextId = contextId;

    if(ecuitem)
    {

        unsigned int serviceID;
        unsigned int size;

        serviceID = serviceId;

        if ((DLT_SERVICE_ID_CALLSW_CINJECTION<= serviceID) && (serviceID!=0))
        {
            DltMessage msg;

            /* initialise new message */
            dlt_message_init(&msg,0);

            // Request parameter:
            // data_length uint32
            // data        uint8[]

            /* prepare payload of data */
            size = (data.size());
            msg.datasize = 4 + 4 + size;
            if (msg.databuffer) free(msg.databuffer);
            msg.databuffer = (uint8_t *) malloc(msg.datasize);

            memcpy(msg.databuffer  , &serviceID,sizeof(serviceID));
            memcpy(msg.databuffer+4, &size, sizeof(size));
            memcpy(msg.databuffer+8, data.constData(), data.size());

            /* send message */
            controlMessage_SendControlMessage(ecuitem,msg,injectionAplicationId,injectionContextId);

            /* free message */
            dlt_message_free(&msg,0);
        }
    }
}

void MainWindow::on_action_menuDLT_Send_Injection_triggered()
{
    /* get selected ECU from configuration */
    EcuItem* ecuitem = 0;
    ApplicationItem* appitem = 0;
    ContextItem* conitem = 0;

    getSelectedItems(&ecuitem,&appitem,&conitem);

    if(!ecuitem)
    {
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("Nothing selected in configuration!"));
        return;
    }
    else
    {
        /* show Injection dialog */
        InjectionDialog dlg("","");
        dlg.updateHistory();

        if(conitem)
        {
            dlg.setApplicationId(appitem->id);
            dlg.setContextId(conitem->id);
        }
        else if(appitem)
        {
            dlg.setApplicationId(appitem->id);
            dlg.setContextId(injectionContextId);
        }
        else
        {
            dlg.setApplicationId(injectionAplicationId);
            dlg.setContextId(injectionContextId);
        }
        dlg.setServiceId(injectionServiceId);
        dlg.setData(injectionData);
        dlg.setDataBinary(injectionDataBinary);

        if(dlg.exec())
        {
            injectionAplicationId = dlg.getApplicationId();
            injectionContextId = dlg.getContextId();
            injectionServiceId = dlg.getServiceId();
            injectionData = dlg.getData();
            injectionDataBinary = dlg.getDataBinary();

            dlg.storeHistory();

            SendInjection(ecuitem);
        }
    }
    //else
    //    QMessageBox::warning(0, QString("DLT Viewer"),
    //                         QString("No ECU selected in configuration!"));
}

void MainWindow::controlMessage_SetApplication(EcuItem *ecuitem, QString apid, QString appdescription)
{
    /* Try to find App */
    for(int numapp = 0; numapp < ecuitem->childCount(); numapp++)
    {
        ApplicationItem * appitem = (ApplicationItem *) ecuitem->child(numapp);

        if(appitem->id == apid)
        {
            appitem->description = appdescription;
            appitem->update();

            return;
        }
    }

    /* No app and no con found */
    ApplicationItem* appitem = new ApplicationItem(ecuitem);
    appitem->id = apid;
    appitem->description = appdescription;
    appitem->update();
    ecuitem->addChild(appitem);
}

void MainWindow::controlMessage_SetContext(EcuItem *ecuitem, QString apid, QString ctid,QString ctdescription,int log_level,int trace_status)
{
    /* First try to find existing context */
    for(int numapp = 0; numapp < ecuitem->childCount(); numapp++)
    {
        ApplicationItem * appitem = (ApplicationItem *) ecuitem->child(numapp);

        for(int numcontext = 0; numcontext < appitem->childCount(); numcontext++)
        {
            ContextItem * conitem = (ContextItem *) appitem->child(numcontext);

            if(appitem->id == apid && conitem->id == ctid)
            {
                /* set new log level and trace status */
                conitem->loglevel = log_level;
                conitem->tracestatus = trace_status;
                conitem->description = ctdescription;
                conitem->status = ContextItem::valid;
                conitem->update();
                return;
            }
        }
    }

    /* Try to find App */
    for(int numapp = 0; numapp < ecuitem->childCount(); numapp++)
    {
        ApplicationItem * appitem = (ApplicationItem *) ecuitem->child(numapp);

        if(appitem->id == apid)
        {
            /* Add new context */
            ContextItem* conitem = new ContextItem(appitem);
            conitem->id = ctid;
            conitem->loglevel = log_level;
            conitem->tracestatus = trace_status;
            conitem->description = ctdescription;
            conitem->status = ContextItem::valid;
            conitem->update();
            appitem->addChild(conitem);

            return;
        }
    }

    /* No app and no con found */
    ApplicationItem* appitem = new ApplicationItem(ecuitem);
    appitem->id = apid;
    appitem->description = QString("");
    appitem->update();
    ecuitem->addChild(appitem);
    ContextItem* conitem = new ContextItem(appitem);
    conitem->id = ctid;
    conitem->loglevel = log_level;
    conitem->tracestatus = trace_status;
    conitem->description = ctdescription;
    conitem->status = ContextItem::valid;
    conitem->update();
    appitem->addChild(conitem);
}

void MainWindow::controlMessage_Timezone(int timezone, unsigned char dst)
{
    if(!project.settings->automaticTimeSettings && project.settings->automaticTimezoneFromDlt)
    {
        project.settings->utcOffset = timezone;
        project.settings->dst = dst;
    }
}

void MainWindow::controlMessage_UnregisterContext(QString ecuId,QString appId,QString ctId)
{
    if(!project.settings->updateContextsUnregister)
        return;

    /* find ecu item */
    EcuItem *ecuitemFound = 0;
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if(ecuitem->id == ecuId)
        {
            ecuitemFound = ecuitem;
            break;
        }
    }

    if(!ecuitemFound)
        return;

    /* First try to find existing context */
    for(int numapp = 0; numapp < ecuitemFound->childCount(); numapp++)
    {
        ApplicationItem * appitem = (ApplicationItem *) ecuitemFound->child(numapp);

        for(int numcontext = 0; numcontext < appitem->childCount(); numcontext++)
        {
            ContextItem * conitem = (ContextItem *) appitem->child(numcontext);

            if(appitem->id == appId && conitem->id == ctId)
            {
                /* remove context */
                delete conitem->parent()->takeChild(conitem->parent()->indexOfChild(conitem));
                return;
            }
        }
    }
}

void MainWindow::on_action_menuHelp_Support_triggered()
{
  QMessageBox msgBox(this);
  msgBox.setWindowTitle("Mail-Support DLT");
  msgBox.setTextFormat(Qt::RichText); //this is what makes the links clickable
  QString text = "<a href='mailto:";
  text.append(DLT_SUPPORT_MAIL_ADDRESS);
  text.append("?Subject=DLT Question: [please add subject] ");//subject
  text.append("&body=Please keep version information in mail:%0D%0ADLT Version: ").append(PACKAGE_VERSION).append("-");//body start
  text.append(PACKAGE_VERSION_STATE);
  text.append("%0D%0ABuild Date: ");
  text.append(__DATE__);
  text.append("-");
  text.append(__TIME__).append("\nQt Version: ").append(QT_VERSION_STR);
  text.append("'");//end body
  text.append("><center>Mailto ").append(DLT_SUPPORT_NAME).append(" DLT-Viewer-Support:<br>");
  text.append(DLT_SUPPORT_MAIL_ADDRESS).append("</center></a>");
  msgBox.setText(text);
  msgBox.setStandardButtons(QMessageBox::Ok);
  msgBox.exec();
}

void MainWindow::on_action_menuHelp_Info_triggered()
{
    QMessageBox::information(0, QString("DLT Viewer"),
                             QString("Package Version : %1 %2\n").arg(PACKAGE_VERSION).arg(PACKAGE_VERSION_STATE)+
                             QString("Package Revision: %1\n\n").arg(PACKAGE_REVISION)+
                             QString("Build Date: %1\n").arg(__DATE__)+
                             QString("Build Time: %1\n").arg(__TIME__)+
                             QString("Qt Version: %1\n\n").arg(QT_VERSION_STR)+
                         #if (BYTE_ORDER==BIG_ENDIAN)
                             QString("Architecture: Big Endian\n\n")+
                         #else
                             QString("Architecture: Little Endian\n\n")+
                         #endif
                             QString("(C) 2010,2014 BMW AG\n"));
}


void MainWindow::on_action_menuHelp_Command_Line_triggered()
{
    // Please copy changes to OptManager::getInstance().cpp - printUsage()

    QMessageBox::information(0, QString("DLT Viewer - Command line usage"),
                         #if (WIN32)
                             QString("Usage: dlt_viewer.exe [OPTIONS]\n\n")+
                             QString("Options:\n")+
                         #else
                             QString("Usage: dlt_viewer [OPTIONS]\n\n")+
                             QString("Options:\n")+
                             QString(" -h \t\tPrint usage\n")+
                         #endif
                             QString(" -s or --silent \t\tEnable silent mode without warning message boxes\n")+
                             QString(" -p projectfile \t\tLoading project file on startup (must end with .dlp)\n")+
                             QString(" -l logfile \t\tLoading logfile on startup (must end with .dlt)\n")+
                             QString(" -f filterfile \t\tLoading filterfile on startup (must end with .dlf)\n")+
                             QString(" -c logfile textfile \tConvert logfile file to textfile (logfile must end with .dlt)\n")+
                             QString(" -e \"plugin|command|param1|..|param<n>\" \tExecute a command plugin with <n> parameters.")
                             );
}

void MainWindow::on_pluginWidget_itemSelectionChanged()
{
    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();

    if((list.count() >= 1) ) {
        ui->action_menuPlugin_Edit->setEnabled(true);
        ui->action_menuPlugin_Hide->setEnabled(true);
        ui->action_menuPlugin_Show->setEnabled(true);
        ui->action_menuPlugin_Disable->setEnabled(true);
    }
}
void MainWindow::on_filterWidget_itemSelectionChanged()
{
    ui->action_menuFilter_Load->setEnabled(true);

    if( project.filter->topLevelItemCount() > 0 ){
        //ui->action_menuFilter_Save_As->setEnabled(true);
        ui->action_menuFilter_Clear_all->setEnabled(true);
    }else{
        //ui->action_menuFilter_Save_As->setEnabled(false);
        ui->action_menuFilter_Clear_all->setEnabled(false);
    }

    if((project.filter->selectedItems().count() >= 1) ) {
        ui->action_menuFilter_Delete->setEnabled(true);
        ui->action_menuFilter_Edit->setEnabled(true);        
        ui->action_menuFilter_Duplicate->setEnabled(true);
    }else{
        ui->action_menuFilter_Delete->setEnabled(false);
        ui->action_menuFilter_Edit->setEnabled(false);        
        ui->action_menuFilter_Duplicate->setEnabled(false);
    }
}

void MainWindow::on_configWidget_itemSelectionChanged()
{
    /* get selected ECU from configuration */
    EcuItem* ecuitem = 0;
    ApplicationItem* appitem = 0;
    ContextItem* conitem = 0;

    getSelectedItems(&ecuitem,&appitem,&conitem);

    ui->action_menuDLT_Get_Default_Log_Level->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Set_Default_Log_Level->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Get_Local_Time_2->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Get_Software_Version->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Store_Config->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Get_Log_Info->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Set_Log_Level->setEnabled(conitem && ecuitem->connected);
    ui->action_menuDLT_Set_All_Log_Levels->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Reset_to_Factory_Default->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Send_Injection->setEnabled(ecuitem && ecuitem->connected && !appitem);
    ui->action_menuDLT_Edit_All_Log_Levels->setEnabled(ecuitem);

    ui->action_menuConfig_Application_Add->setEnabled(ecuitem && !appitem);
    ui->action_menuConfig_Application_Edit->setEnabled(appitem && !conitem);
    ui->action_menuConfig_Application_Delete->setEnabled(appitem && !conitem);
    ui->action_menuConfig_Context_Add->setEnabled(appitem && !conitem);
    ui->action_menuConfig_Context_Edit->setEnabled(conitem);
    ui->action_menuConfig_Context_Delete->setEnabled(conitem);
    ui->action_menuConfig_ECU_Add->setEnabled(true);
    ui->action_menuConfig_ECU_Edit->setEnabled(ecuitem && !appitem);
    ui->action_menuConfig_ECU_Delete->setEnabled(ecuitem && !appitem);
    ui->action_menuConfig_Delete_All_Contexts->setEnabled(ecuitem && !appitem);
    ui->action_menuConfig_Connect->setEnabled(ecuitem && !appitem && !ecuitem->tryToConnect);
    ui->action_menuConfig_Disconnect->setEnabled(ecuitem && !appitem && ecuitem->tryToConnect);
    ui->action_menuConfig_Expand_All_ECUs->setEnabled(ecuitem && !appitem );
    ui->action_menuConfig_Collapse_All_ECUs->setEnabled(ecuitem && !appitem );

}

void MainWindow::updateScrollButton()
{
    // Mapping: variable to button
    scrollButton->setChecked(settings->autoScroll);

    // inform plugins about changed autoscroll status
    pluginManager.autoscrollStateChanged(settings->autoScroll);
}


void MainWindow::updateRecentFileActions()
{
    int numRecentFiles = qMin(recentFiles.size(), (int)MaxRecentFiles);

    for (int i = 0; i < numRecentFiles; ++i) {
        QString text = tr("&%1 %2").arg(i + 1).arg(recentFiles[i]);
        recentFileActs[i]->setText(text);
        recentFileActs[i]->setData(recentFiles[i]);
        recentFileActs[i]->setVisible(true);
    }
    for (int j = numRecentFiles; j < MaxRecentFiles; ++j)
        recentFileActs[j]->setVisible(false);

    ui->menuRecent_files->setEnabled(recentFiles.size()>0);
}

void MainWindow::setCurrentFile(const QString &fileName)
{
    recentFiles.removeAll(fileName);
    recentFiles.prepend(fileName);
    while (recentFiles.size() > MaxRecentFiles)
        recentFiles.removeLast();

    updateRecentFileActions();

    // write settings
    DltSettingsManager::getInstance()->setValue("other/recentFileList",recentFiles);
}

void MainWindow::removeCurrentFile(const QString &fileName)
{
    recentFiles.removeAll(fileName);
    updateRecentFileActions();

    // write settings
    DltSettingsManager::getInstance()->setValue("other/recentFileList",recentFiles);
}

void MainWindow::openRecentProject()
{
    QAction *action = qobject_cast<QAction *>(sender());
    QString projectName;

    if (action)
    {
        projectName = action->data().toString();

        /* Open existing project */
        if(!projectName.isEmpty() && openDlpFile(projectName))
        {
           //thats it.
        }
        else
        {
            removeCurrentProject(projectName);
            return;
        }
    }
}

void MainWindow::updateRecentProjectActions()
{
    int numRecentProjects = qMin(recentProjects.size(), (int)MaxRecentProjects);

    for (int i = 0; i < numRecentProjects; ++i) {
        QString text = tr("&%1 %2").arg(i + 1).arg(recentProjects[i]);
        recentProjectActs[i]->setText(text);
        recentProjectActs[i]->setData(recentProjects[i]);
        recentProjectActs[i]->setVisible(true);
    }
    for (int j = numRecentProjects; j < MaxRecentProjects; ++j)
        recentProjectActs[j]->setVisible(false);

    ui->menuRecent_projects->setEnabled(recentProjects.size()>0);
}

void MainWindow::setCurrentProject(const QString &projectName)
{
    recentProjects.removeAll(projectName);
    recentProjects.prepend(projectName);
    while (recentProjects.size() > MaxRecentProjects)
        recentProjects.removeLast();

    updateRecentProjectActions();

    // write settings
    DltSettingsManager::getInstance()->setValue("other/recentProjectList",recentProjects);
}

void MainWindow::removeCurrentProject(const QString &projectName)
{
    recentProjects.removeAll(projectName);
    updateRecentProjectActions();

    // write settings
    DltSettingsManager::getInstance()->setValue("other/recentProjectList",recentProjects);
}


void MainWindow::openRecentFilters()
{
    QAction *action = qobject_cast<QAction *>(sender());
    QString fileName;

    if (action)
    {
        fileName = action->data().toString();

        openDlfFile(fileName,true);
    }
}

void MainWindow::updateRecentFiltersActions()
{
    int numRecentFilters = qMin(recentFilters.size(), (int)MaxRecentFilters);

    for (int i = 0; i < numRecentFilters; ++i) {
        QString text = tr("&%1 %2").arg(i + 1).arg(recentFilters[i]);

        recentFiltersActs[i]->setText(text);
        recentFiltersActs[i]->setData(recentFilters[i]);
        recentFiltersActs[i]->setVisible(true);
    }

    for (int j = numRecentFilters; j < MaxRecentFilters; ++j)
    {
        recentFiltersActs[j]->setVisible(false);
    }

    ui->menuRecent_Filters->setEnabled(recentFilters.size()>0);
}

void MainWindow::setCurrentFilters(const QString &filtersName)
{
    recentFilters.removeAll(filtersName);
    recentFilters.prepend(filtersName);
    while (recentFilters.size() > MaxRecentFilters)
        recentFilters.removeLast();

    updateRecentFiltersActions();

    // write settings
    DltSettingsManager::getInstance()->setValue("other/recentFiltersList",recentFilters);
}

void MainWindow::removeCurrentFilters(const QString &filtersName)
{
    recentFilters.removeAll(filtersName);
    updateRecentFiltersActions();

    // write settings
    DltSettingsManager::getInstance()->setValue("other/recentFiltersList",filtersName);
}


void MainWindow::setCurrentHostname(const QString &hostName)
{
    recentHostnames.removeAll(hostName);
    recentHostnames.prepend(hostName);
    while (recentHostnames.size() > MaxRecentHostnames)
        recentHostnames.removeLast();

    /* Write settings for recent hostnames*/
    DltSettingsManager::getInstance()->setValue("other/recentHostnameList",recentHostnames);
}

void MainWindow::setCurrentPort(const QString &portName)
{
    recentPorts.removeAll(portName);
    recentPorts.prepend(portName);
    while (recentPorts.size() > MaxRecentPorts)
        recentPorts.removeLast();

    /* Write settings for recent ports */
    DltSettingsManager::getInstance()->setValue("other/recentPortList",recentPorts);
}

void MainWindow::tableViewValueChanged(int value)
{
    int maximum = ((QAbstractSlider *)(ui->tableView->verticalScrollBar()))->maximum();

    if (value==maximum)
    {
        /* Only enable, if disabled */
        if (settings->autoScroll==Qt::Unchecked)
        {
            /* do not automatically enable scrolling when scrolling to bottom */
            //on_actionAutoScroll_triggered(Qt::Checked);
            //updateScrollButton();
        }
    }
    else
    {
        /* Only disable, if enabled */
        if (settings->autoScroll==Qt::Checked)
        {
            /* disable scrolling */
            on_actionAutoScroll_triggered(Qt::Unchecked);
            updateScrollButton();
        }
    }
}

void MainWindow::sendUpdates(EcuItem* ecuitem)
{
    /* update default log level, trace status and timing packets */
    if (ecuitem->sendDefaultLogLevel)
    {
        controlMessage_SetDefaultLogLevel(ecuitem,ecuitem->loglevel);
        controlMessage_SetDefaultTraceStatus(ecuitem,ecuitem->tracestatus);
        controlMessage_SetVerboseMode(ecuitem,ecuitem->verbosemode);
    }

    controlMessage_SetTimingPackets(ecuitem,ecuitem->timingPackets);

    if (ecuitem->sendGetLogInfo)
        controlMessage_GetLogInfo(ecuitem);

    /* update status */
    ecuitem->status = EcuItem::valid;
    ecuitem->update();

}

void MainWindow::stateChangedSerial(bool dsrChanged){
    /* signal emited when connection state changed */

    /* find socket which emited signal */
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if( ecuitem->m_serialport == sender())
        {
            /* update ECU item */
            ecuitem->update();

            if(dsrChanged)
            {
                /* send new default log level to ECU, if selected in dlg */
                if (ecuitem->updateDataIfOnline)
                {
                    sendUpdates(ecuitem);
                }
            }

            if(dsrChanged)
            {
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionOnline);
            }
            else
            {
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionOffline);
            }

        }
    }
}

void MainWindow::stateChangedTCP(QAbstractSocket::SocketState socketState)
{
    /* signal emited when connection state changed */

    /* find socket which emited signal */
    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);
        if( &(ecuitem->socket) == sender())
        {
            /* update ECU item */
            ecuitem->update();

            if (socketState==QAbstractSocket::ConnectedState)
            {
                /* send new default log level to ECU, if selected in dlg */
                if (ecuitem->updateDataIfOnline)
                {
                    sendUpdates(ecuitem);
                }
            }

            switch(socketState){
            case QAbstractSocket::UnconnectedState:
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionOffline);
                break;
            case QAbstractSocket::ConnectingState:
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionConnecting);
                break;
            case QAbstractSocket::ConnectedState:
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionOnline);
                break;
            case QAbstractSocket::ClosingState:
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionOffline);
                break;
            default:
                pluginManager.stateChanged(num,QDltConnection::QDltConnectionOffline);
                break;
            }
        }
    }
}

//----------------------------------------------------------------------------
// Search functionalities
//----------------------------------------------------------------------------

void MainWindow::on_action_menuSearch_Find_triggered()
{

    searchDlg->open();
    searchDlg->selectText();
}

//----------------------------------------------------------------------------
// Plugin functionalities
//----------------------------------------------------------------------------

void MainWindow::loadPlugins()
{
    /* load plugins from subdirectory plugins, from directory if set in settings and from /usr/share/dlt-viewer/plugins in Linux */
    if(settings->pluginsPath)
        pluginManager.loadPlugins(settings->pluginsPathName);
    else
        pluginManager.loadPlugins(QString());

    /* update plugin widgets */
    QList<QDltPlugin*> plugins = pluginManager.getPlugins();
    for (int idx = 0; idx < plugins.size();idx++ )
    {
      QDltPlugin* plugin = plugins[idx];

      PluginItem* item = new PluginItem(0,plugin);

      plugin->setMode((QDltPlugin::Mode) DltSettingsManager::getInstance()->value("plugin/pluginmodefor"+plugin->getName(),QVariant(QDltPlugin::ModeDisable)).toInt());

      if(plugin->isViewer())
      {
        item->widget = plugin->initViewer();
        item->dockWidget = new MyPluginDockWidget(item,this);
        item->dockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
        item->dockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
        item->dockWidget->setWidget(item->widget);
        item->dockWidget->setObjectName(plugin->getName());

        addDockWidget(Qt::LeftDockWidgetArea, item->dockWidget);

        if(plugin->getMode() != QDltPlugin::ModeShow)
        {
            item->dockWidget->hide();
        }
      }

      item->update();
      project.plugin->addTopLevelItem(item);

    }

    /* initialise control interface */
    pluginManager.initControl(&qcontrol);
}

void MainWindow::updatePluginsECUList()
{
    QStringList list;

    for(int num = 0; num < project.ecu->topLevelItemCount (); num++)
    {
        EcuItem *ecuitem = (EcuItem*)project.ecu->topLevelItem(num);

        list.append(ecuitem->id + " (" + ecuitem->description + ")");
    }
    pluginManager.initConnections(list);
}

void MainWindow::updatePlugins() {
    for(int num = 0; num < project.plugin->topLevelItemCount (); num++) {
        PluginItem *item = (PluginItem*)project.plugin->topLevelItem(num);

        updatePlugin(item);
    }

}

void MainWindow::updatePlugin(PluginItem *item) {
    item->takeChildren();

    bool ret = item->getPlugin()->loadConfig(item->getFilename());
    QString err_text = item->getPlugin()->error();
	//We should not need error handling when disabling the plugins. But why is loadConfig called then anyway?
    if (item->getMode() != QDltPlugin::ModeDisable)
    {
       
        if ( false == ret )
        {
            QString err_header = "Plugin Error: ";
            err_header.append(item->getName());
            QString err_body = err_header;
            err_body.append(" returned error:\n");
            err_body.append(err_text);
            err_body.append("\nin loadConfig!");
            ErrorMessage(QMessageBox::Critical,err_header,err_body);
            //item->setMode(QDltPlugin::ModeDisable);
        }
        else if ( 0 < err_text.length() )
        {
			//we have no error, but the plugin complains about something
            QString err_header = "Plugin Warning: ";
            err_header.append(item->getName());
            QString err_body = err_header;
            err_body.append(" returned message:\n");
            err_body.append(err_text);
            err_body.append("\nin loadConfig. ");
            ErrorMessage(QMessageBox::Warning,err_header,err_body);
        }
    }


    QStringList list = item->getPlugin()->infoConfig();
    for(int num=0;num<list.size();num++) {
        item->addChild(new QTreeWidgetItem(QStringList(list.at(num))));
    }

    item->update();

    if(item->dockWidget) {
        if(item->getMode() == QDltPlugin::ModeShow) {
            item->dockWidget->show();
        }
        else {
            item->dockWidget->hide();
        }
    }
}

void MainWindow::versionString(QDltMsg &msg)
{
    // get the version string from the version message
    // Skip the ServiceID, Status and Length bytes and start from the String containing the ECU Software Version
    QByteArray payload = msg.getPayload();
    QByteArray data = payload.mid(9,(payload.size()>262)?256:(payload.size()-9));
    QString version = msg.toAscii(data,true);
    version = version.trimmed(); // remove all white spaces at beginning and end
    qDebug() << "AutoloadPlugins Version:" << version;
    autoloadPluginsVersionStrings.append(version);
    statusFileVersion->setText("Version: "+autoloadPluginsVersionStrings.join(" "));

    if(settings->pluginsAutoloadPath)
    {
        pluginsAutoload(version);
    }
}

void MainWindow::pluginsAutoload(QString version)
{
    // Iterate through all enabled decoder plugins
    for(int num = 0; num < project.plugin->topLevelItemCount(); num++) {
        PluginItem *item = (PluginItem*)project.plugin->topLevelItem(num);

        if(item->getMode() != QDltPlugin::ModeDisable && item->getPlugin()->isDecoder())
        {
            QString searchPath = settings->pluginsAutoloadPathName+ "/" + item->getName();

            qDebug() << "AutoloadPlugins Search:" << searchPath;

            // search for files in plugin directory which contains version string
            QStringList nameFilter("*"+version+"*");
            QDir directory(searchPath);
            QStringList txtFilesAndDirectories = directory.entryList(nameFilter);
            if(txtFilesAndDirectories.size()>1)
                txtFilesAndDirectories.sort(); // sort if several files are found

            if(!txtFilesAndDirectories.isEmpty() )
            {
                // file with version string found
                QString filename = searchPath + "/" + txtFilesAndDirectories[0];

                // check if filename already loaded
                if(item->getFilename()!=filename)
                {
                    qDebug() << "AutoloadPlugins Load:" << filename;

                    // load new configuration
                    item->setFilename(filename);
                    item->getPlugin()->loadConfig(filename);
                    item->update();
                }
                else
                {
                    qDebug() << "AutoloadPlugins already loaded:" << filename;
                }
            }
        }
    }

}

void MainWindow::on_action_menuPlugin_Edit_triggered() {
    /* get selected plugin */
    bool callInitFile = false;

    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();
    if((list.count() == 1) ) {
        QTreeWidgetItem *treeitem = list.at(0);
        if(treeitem->parent())
        {
            /* This is not a plugin item */
            return;
        }
        PluginItem* item = (PluginItem*) treeitem;

        /* show plugin dialog */
        PluginDialog dlg;
        dlg.setName(item->getName());
        dlg.setPluginVersion(item->getPluginVersion());
        dlg.setPluginInterfaceVersion(item->getPluginInterfaceVersion());
        dlg.setFilename(item->getFilename());
        dlg.setMode(item->getMode());
        if(!item->getPlugin()->isViewer())
            dlg.removeMode(2); // remove show mode, if no viewer plugin
        dlg.setType(item->getType());
        if(dlg.exec()) {
            /* Check if there was a change that requires a refresh */
            if(item->getMode() != dlg.getMode())
                callInitFile = true;
            if(item->getMode() == QDltPlugin::ModeShow && dlg.getMode() != QDltPlugin::ModeDisable)
                callInitFile = false;
            if(dlg.getMode() == QDltPlugin::ModeShow && item->getMode() != QDltPlugin::ModeDisable)
                callInitFile = false;
            if(item->getFilename() != dlg.getFilename())
                callInitFile = true;

            item->setFilename( dlg.getFilename() );
            item->setMode( dlg.getMode() );
            item->setType( dlg.getType() );

            /* update plugin item */
            updatePlugin(item);
            item->savePluginModeToSettings();
        }
        if(callInitFile)
        {
            applyConfigEnabled(true);
        }
    }
    else
    {
        ErrorMessage(QMessageBox::Warning,QString("DLT Viewer"),QString("No Plugin selected!"));
    }

}

void MainWindow::on_action_menuPlugin_Show_triggered() {

    /* get selected plugin */
    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();
    if((list.count() == 1) ) {
        PluginItem* item = (PluginItem*) list.at(0);

        if(item->getMode() != QDltPlugin::ModeShow){
            int oldMode = item->getMode();

            item->setMode( QDltPlugin::ModeShow );
            item->savePluginModeToSettings();
            updatePlugin(item);

            if(oldMode == QDltPlugin::ModeDisable){
                applyConfigEnabled(true);
            }
        }else{
             ErrorMessage(QMessageBox::Warning,QString("DLT Viewer"),QString("The selected Plugin is already active."));
        }
    }
    else {
        ErrorMessage(QMessageBox::Warning,QString("DLT Viewer"),QString("No Plugin selected!"));
    }

}

void MainWindow::on_action_menuPlugin_Hide_triggered() {
    /* get selected plugin */
    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();
    if((list.count() == 1) ) {
        PluginItem* item = (PluginItem*) list.at(0);

        if(item->getMode() == QDltPlugin::ModeShow){
            item->setMode( QDltPlugin::ModeEnable );
            item->savePluginModeToSettings();
            updatePlugin(item);
        }else{
            ErrorMessage(QMessageBox::Warning,QString("DLT Viewer"),QString("No Plugin selected!"));

            QMessageBox::warning(0, QString("DLT Viewer"),
                                 QString("The selected Plugin is already hidden or deactivated."));
        }
    }
    else {
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Plugin selected!"));
    }

}

void MainWindow::action_menuPlugin_Enable_triggered()
{
    /* get selected plugin */
    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();
    if((list.count() == 1) ) {
        PluginItem* item = (PluginItem*) list.at(0);

        if(item->getMode() == QDltPlugin::ModeDisable){
            item->setMode( QDltPlugin::ModeEnable );
            item->savePluginModeToSettings();
            updatePlugin(item);
            applyConfigEnabled(true);
        }else{
            QMessageBox::warning(0, QString("DLT Viewer"),
                                 QString("The selected Plugin is already deactivated."));
        }
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Plugin selected!"));
}

void MainWindow::on_action_menuPlugin_Disable_triggered()
{
    /* get selected plugin */
    QList<QTreeWidgetItem *> list = project.plugin->selectedItems();
    if((list.count() == 1) ) {
        PluginItem* item = (PluginItem*) list.at(0);

        if(item->getMode() != QDltPlugin::ModeDisable){
            item->setMode( QDltPlugin::ModeDisable );
            item->savePluginModeToSettings();
            updatePlugin(item);
            applyConfigEnabled(true);
        }else{
            QMessageBox::warning(0, QString("DLT Viewer"),
                                 QString("The selected Plugin is already deactivated."));
        }
    }
    else
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Plugin selected!"));
}

//----------------------------------------------------------------------------
// Filter functionalities
//----------------------------------------------------------------------------

void MainWindow::filterAddTable() {
    QModelIndexList list = ui->tableView->selectionModel()->selection().indexes();
    QDltMsg msg;
    QByteArray data;

    if(list.count()<=0)
    {
        QMessageBox::critical(0, QString("DLT Viewer"),
                              QString("No message selected"));
        return;
    }

    QModelIndex index;
    for(int num=0; num < list.count();num++)
    {
        index = list[num];
        if(index.column()==0)
        {
            break;
        }
    }

    data = qfile.getMsgFilter(index.row());
    msg.setMsg(data);

    /* decode message if necessary */
    iterateDecodersForMsg(msg,!OptManager::getInstance()->issilentMode());

    /* show filter dialog */
    FilterDialog dlg;
    dlg.setEnableEcuId(!msg.getEcuid().isEmpty());
    dlg.setEcuId(msg.getEcuid());
    dlg.setEnableApplicationId(!msg.getApid().isEmpty());
    dlg.setApplicationId(msg.getApid());
    dlg.setEnableContextId(!msg.getCtid().isEmpty());
    dlg.setContextId(msg.getCtid());
    dlg.setHeaderText(msg.toStringHeader());
    dlg.setPayloadText(msg.toStringPayload());

    if(dlg.exec()==1) {
        FilterItem* item = new FilterItem(0);
        project.filter->addTopLevelItem(item);
        filterDialogRead(dlg,item);
    }
}

void MainWindow::filterAdd() {
    EcuItem* ecuitem = 0;
    ContextItem* conitem = 0;
    ApplicationItem* appitem = 0;

    /* add filter triggered from popupmenu in Context list */
    /* get selected context from configuration */
    QList<QTreeWidgetItem *> list = project.ecu->selectedItems();
    if((list.count() == 1) && (list.at(0)->type() == ecu_type))
    {
        ecuitem = (EcuItem*) list.at(0);
    }
    if((list.count() == 1) && (list.at(0)->type() == application_type))
    {
        appitem = (ApplicationItem*) list.at(0);
        ecuitem = (EcuItem*) appitem->parent();
    }
    if((list.count() == 1) && (list.at(0)->type() == context_type))
    {
        conitem = (ContextItem*) list.at(0);
        appitem = (ApplicationItem*) conitem->parent();
        ecuitem = (EcuItem*) appitem->parent();
    }

    /* show filter dialog */
    FilterDialog dlg;

    if(ecuitem)
    {
        dlg.setEnableEcuId(true);
        dlg.setEcuId(ecuitem->id);
    }

    if(appitem)
    {
        dlg.setEnableApplicationId(true);
        dlg.setApplicationId(appitem->id);
    }

    if(conitem)
    {
        dlg.setEnableContextId(true);
        dlg.setContextId(conitem->id);
    }

    if(dlg.exec()==1) {
        FilterItem* item = new FilterItem(0);
        project.filter->addTopLevelItem(item);
        filterDialogRead(dlg,item);
    }
}

void MainWindow::on_action_menuFilter_Save_As_triggered()
{

    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Save DLT Filters"), workingDirectory.getDlfDirectory(), tr("DLT Filter File (*.dlf);;All files (*.*)"));

    if(!fileName.isEmpty())
    {
        workingDirectory.setDlfDirectory(QFileInfo(fileName).absolutePath());
        project.SaveFilter(fileName);
        setCurrentFilters(fileName);
    }
}


void MainWindow::on_action_menuFilter_Load_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Load DLT Filter file"), workingDirectory.getDlfDirectory(), tr("DLT Filter Files (*.dlf);;All files (*.*)"));

    openDlfFile(fileName,true);
}

void MainWindow::on_action_menuFilter_Add_triggered() {
    /* show filter dialog */
    FilterDialog dlg;

    if(dlg.exec()==1) {
        FilterItem* item = new FilterItem(0);
        project.filter->addTopLevelItem(item);
        filterDialogRead(dlg,item);
    }
}

void MainWindow::filterDialogWrite(FilterDialog &dlg,FilterItem* item)
{
    dlg.setType((int)(item->filter.type));

    dlg.setName(item->filter.name);
    dlg.setEcuId(item->filter.ecuid);
    dlg.setApplicationId(item->filter.apid);
    dlg.setContextId(item->filter.ctid);
    dlg.setHeaderText(item->filter.header);
    dlg.setPayloadText(item->filter.payload);

    dlg.setEnableRegexp(item->filter.enableRegexp);
    dlg.setActive(item->filter.enableFilter);
    dlg.setEnableEcuId(item->filter.enableEcuid);
    dlg.setEnableApplicationId(item->filter.enableApid);
    dlg.setEnableContextId(item->filter.enableCtid);
    dlg.setEnableHeaderText(item->filter.enableHeader);
    dlg.setEnablePayloadText(item->filter.enablePayload);
    dlg.setEnableCtrlMsgs(item->filter.enableCtrlMsgs);
    dlg.setEnableLogLevelMax(item->filter.enableLogLevelMax);
    dlg.setEnableLogLevelMin(item->filter.enableLogLevelMin);
    dlg.setEnableMarker(item->filter.enableMarker);

    dlg.setFilterColour(item->filter.filterColour);

    dlg.setLogLevelMax(item->filter.logLevelMax);
    dlg.setLogLevelMin(item->filter.logLevelMin);
}

void MainWindow::filterDialogRead(FilterDialog &dlg,FilterItem* item)
{
    item->filter.type = (QDltFilter::FilterType)(dlg.getType());

    item->filter.name = dlg.getName();

    item->filter.ecuid = dlg.getEcuId();
    item->filter.apid = dlg.getApplicationId();
    item->filter.ctid = dlg.getContextId();
    item->filter.header = dlg.getHeaderText();
    item->filter.payload = dlg.getPayloadText();

    item->filter.enableRegexp = dlg.getEnableRegexp();
    item->filter.enableFilter = dlg.getEnableActive();
    item->filter.enableEcuid = dlg.getEnableEcuId();
    item->filter.enableApid = dlg.getEnableApplicationId();
    item->filter.enableCtid = dlg.getEnableContextId();
    item->filter.enableHeader = dlg.getEnableHeaderText();
    item->filter.enablePayload = dlg.getEnablePayloadText();
    item->filter.enableCtrlMsgs = dlg.getEnableCtrlMsgs();
    item->filter.enableLogLevelMax = dlg.getEnableLogLevelMax();
    item->filter.enableLogLevelMin = dlg.getEnableLogLevelMin();
    item->filter.enableMarker = dlg.getEnableMarker();

    item->filter.filterColour = dlg.getFilterColour();
    item->filter.logLevelMax = dlg.getLogLevelMax();
    item->filter.logLevelMin = dlg.getLogLevelMin();

    /* update filter item */
    item->update();
    on_filterWidget_itemSelectionChanged();

    /* Update filters in qfile and either update
     * view or pulse the button depending on if it is a filter or
     * marker. */
    filterUpdate();
    if(item->filter.isPositive() || item->filter.isNegative())
    {
        applyConfigEnabled(true);
    }
    if(item->filter.isMarker())
    {
        tableModel->modelChanged();
    }
}

void MainWindow::on_action_menuFilter_Duplicate_triggered() {
    QTreeWidget *widget;

    /* get currently visible filter list in user interface */
    if(ui->tabPFilter->isVisible()) {
        widget = project.filter;
    }
    else
        return;

    /* get selected filter form list */
    QList<QTreeWidgetItem *> list = widget->selectedItems();
    if((list.count() == 1) ) {
        FilterItem* item = (FilterItem*) list.at(0);

        /* show filter dialog */
        FilterDialog dlg;
        filterDialogWrite(dlg,item);
        if(dlg.exec())
        {
            FilterItem* newitem = new FilterItem(0);
            project.filter->addTopLevelItem(newitem);
            filterDialogRead(dlg,newitem);
        }
    }
    else {
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Filter selected!"));
    }
}

void MainWindow::on_action_menuFilter_Edit_triggered() {
    QTreeWidget *widget;

    /* get currently visible filter list in user interface */
    if(ui->tabPFilter->isVisible()) {
        widget = project.filter;
    }
    else
        return;

    /* get selected filter form list */
    QList<QTreeWidgetItem *> list = widget->selectedItems();
    if((list.count() == 1) ) {
        FilterItem* item = (FilterItem*) list.at(0);

        /* show filter dialog */
        FilterDialog dlg;
        filterDialogWrite(dlg,item);
        if(dlg.exec())
        {
            filterDialogRead(dlg,item);
        }
    }
    else {
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Filter selected!"));
    }
}

void MainWindow::on_action_menuFilter_Delete_triggered() {
    QTreeWidget *widget;

    /* get currently visible filter list in user interface */
    if(ui->tabPFilter->isVisible()) {
        widget = project.filter;
    }
    else
        return;

    /* get selected filter from list */
    QList<QTreeWidgetItem *> list = widget->selectedItems();
    if((list.count() == 1) ) {
        /* delete filter */
        FilterItem *item = (FilterItem *)widget->takeTopLevelItem(widget->indexOfTopLevelItem(list.at(0)));
        filterUpdate();
        if(item->filter.isMarker())
        {
            tableModel->modelChanged();
        }
        else
        {
            applyConfigEnabled(true);
        }
        delete widget->takeTopLevelItem(widget->indexOfTopLevelItem(list.at(0)));
    }
    else {
        QMessageBox::warning(0, QString("DLT Viewer"),
                             QString("No Filter selected!"));
    }

    on_filterWidget_itemSelectionChanged();
}

void MainWindow::on_action_menuFilter_Clear_all_triggered() {
    /* delete complete filter list */
    project.filter->clear();
    applyConfigEnabled(true);
}

void MainWindow::filterUpdate() {
    QDltFilter *filter;

    /* update all filters from filter configuration to DLT filter list */

    /* clear old filter list */
    qfile.clearFilter();

    /* iterate through all filters */
    for(int num = 0; num < project.filter->topLevelItemCount (); num++)
    {
        FilterItem *item = (FilterItem*)project.filter->topLevelItem(num);

        filter = new QDltFilter();
        *filter = item->filter;

        if(item->filter.isMarker())
        {
            item->setBackground(0,item->filter.filterColour);
            item->setBackground(1,item->filter.filterColour);
            item->setForeground(0,DltUiUtils::optimalTextColor(item->filter.filterColour));
            item->setForeground(1,DltUiUtils::optimalTextColor(item->filter.filterColour));
        }
        else
        {
            item->setBackground(0,QColor(0xff,0xff,0xff));
            item->setBackground(1,QColor(0xff,0xff,0xff));
            item->setForeground(0,DltUiUtils::optimalTextColor(QColor(0xff,0xff,0xff)));
            item->setForeground(1,DltUiUtils::optimalTextColor(QColor(0xff,0xff,0xff)));
        }

        if(filter->enableRegexp)
        {
            if(!filter->compileRegexps())
            {
                // This is also validated in the UI part
                qDebug() << "Error compiling a regexp" << endl;
            }
        }

        qfile.addFilter(filter);
    }
    qfile.updateSortedFilter();
}



void MainWindow::on_tableView_customContextMenuRequested(QPoint pos)
{
    /* show custom pop menu  for configuration */
    QPoint globalPos = ui->tableView->mapToGlobal(pos);
    QMenu menu(ui->tableView);
    QAction *action;
    QModelIndexList list = ui->tableView->selectionModel()->selection().indexes();

    action = new QAction("&Copy Selection to Clipboard", this);
    connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuConfig_Copy_to_clipboard_triggered()));
    menu.addAction(action);

    menu.addSeparator();

    action = new QAction("&Export...", this);
    if(qfile.sizeFilter() <= 0)
        action->setEnabled(false);
    else
        connect(action, SIGNAL(triggered()), this, SLOT(on_actionExport_triggered()));
    menu.addAction(action);

    menu.addSeparator();

    action = new QAction("&Filter Add", this);
    connect(action, SIGNAL(triggered()), this, SLOT(filterAddTable()));
    menu.addAction(action);

    menu.addSeparator();

    action = new QAction("Load Filter(s)...", this);
    connect(action, SIGNAL(triggered()), this, SLOT(on_action_menuFilter_Load_triggered()));
    menu.addAction(action);

    /* show popup menu */
    menu.exec(globalPos);
}

void MainWindow::keyPressEvent ( QKeyEvent * event )
{
    if(event->matches(QKeySequence::Copy))
    {
        exportSelection(true,false);
    }
    if(event->matches(QKeySequence::Paste))
    {
        QMessageBox::warning(this, QString("Paste"),
                             QString("pressed"));
    }
    if(event->matches(QKeySequence::Cut))
    {
        QMessageBox::warning(this, QString("Cut"),
                             QString("pressed"));
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QString filename;
    QStringList filenames;

    if (event->mimeData()->hasUrls())
    {
        for(int num = 0;num<event->mimeData()->urls().size();num++)
        {
            QUrl url = event->mimeData()->urls()[num];
            filename = url.toLocalFile();

            if(filename.endsWith(".dlt", Qt::CaseInsensitive))
            {
                filenames.append(filename);
                workingDirectory.setDltDirectory(QFileInfo(filename).absolutePath());
            }
            else if(filename.endsWith(".dlp", Qt::CaseInsensitive))
            {
                /* Project file dropped */
                openDlpFile(filename);
            }
            else if(filename.endsWith(".dlf", Qt::CaseInsensitive))
            {
                /* Filter file dropped */
                openDlfFile(filename,true);
            }
            else
            {
                /* ask for active decoder plugin to load configuration */
                QStringList items;
                QList<QDltPlugin*> list = pluginManager.getDecoderPlugins();
                for(int num=0;num<list.size();num++)
                {
                    items << list[num]->getName();
                }

                /* check if decoder plugin list is empty */
                if(list.size()==0)
                {
                    /* show warning */
                    QMessageBox::warning(this, QString("Drag&Drop"),
                                         QString("No decoder plugin active to load configuration of file:\n")+filename);
                    return;
                }

                bool ok;
                QString item = QInputDialog::getItem(this, tr("DLT Viewer"),
                                                         tr("Select Plugin to load configuration:"), items, 0, false, &ok);
                if (ok && !item.isEmpty())
                {
                    QDltPlugin* plugin = pluginManager.findPlugin(item);
                    if(plugin)
                    {
                        plugin->loadConfig(filename);
                        for(int num = 0; num < project.plugin->topLevelItemCount (); num++)
                        {
                            PluginItem *pluginitem = (PluginItem*)project.plugin->topLevelItem(num);
                            if(pluginitem->getPlugin() == plugin)
                            {
                                /* update plugin */
                                pluginitem->setFilename( filename );

                                /* update plugin item */
                                updatePlugin(pluginitem);
                                applyConfigEnabled(true);

                                ui->tabWidget->setCurrentWidget(ui->tabPlugin);

                                break;
                            }
                        }
                    }
                }
            }
        }
        if(!filenames.isEmpty())
        {
            /* DLT log file dropped */
            openDltFile(filenames);
            outputfileIsTemporary = false;
            outputfileIsFromCLI   = false;
        }
    }
    else
    {
        QMessageBox::warning(this, QString("Drag&Drop"),
                             QString("No dlt file or project file or other file dropped!\n")+filename);
    }
}

void MainWindow::sectionInTableDoubleClicked(int logicalIndex){
    ui->tableView->resizeColumnToContents(logicalIndex);
}

void MainWindow::on_pluginWidget_itemExpanded(QTreeWidgetItem* item)
{
    PluginItem *plugin = (PluginItem*)item;
    plugin->takeChildren();
    QStringList list = plugin->getPlugin()->infoConfig();
    for(int num=0;num<list.size();num++) {
        plugin->addChild(new QTreeWidgetItem(QStringList(list.at(num))));
    }
}

void MainWindow::on_filterWidget_itemClicked(QTreeWidgetItem *item, int column)
{
    on_filterWidget_itemSelectionChanged();

    if(column == 0)
    {
        FilterItem *tmp = (FilterItem*)item;
        if(tmp->checkState(column) == Qt::Unchecked)
        {
            tmp->filter.enableFilter = false;
        }
        else
        {
            tmp->filter.enableFilter = true;
        }
        applyConfigEnabled(true);
    }
}

void MainWindow::iterateDecodersForMsg(QDltMsg &msg, int triggeredByUser)
{
    pluginManager.decodeMsg(msg,triggeredByUser);
}

void MainWindow::on_action_menuConfig_Collapse_All_ECUs_triggered()
{
    ui->configWidget->collapseAll();
}

void MainWindow::on_action_menuConfig_Expand_All_ECUs_triggered()
{
    ui->configWidget->expandAll();
}

void MainWindow::on_action_menuConfig_Copy_to_clipboard_triggered()
{
    exportSelection(true,false);
}

void MainWindow::on_action_menuFilter_Append_Filters_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Load DLT Filter file"), workingDirectory.getDlfDirectory(), tr("DLT Filter Files (*.dlf);;All files (*.*)"));

    openDlfFile(fileName,false);
}

int MainWindow::nearest_line(int line){

    if (line < 0 || line > qfile.size()-1){
        return -1;
    }

    // If filters are off, just go directly to the row
    int row = 0;
    if(!qfile.isFilter())
    {
        row = line;
    }
    else
    {
        /* Iterate through filter index, trying to find
         * matching index. If it cannot be found, just settle
         * for the last one that we saw before going over */
        int lastFound = 0, i;
        for(i=0;i<qfile.sizeFilter();i++)
        {
            if(qfile.getMsgFilterPos(i) < line)
            {
                // Track previous smaller
                lastFound = i;
            }
            else if(qfile.getMsgFilterPos(i) == line)
            {
                // It's actually visible
                lastFound = i;
                break;
            }
            else if(qfile.getMsgFilterPos(i) > line)
            {
                // Ok, we went past it, use the last one.
                break;
            }
        }
        row = lastFound;
    }
    return row;
}

void MainWindow::jumpToMsgSignal(int index)
{
    jump_to_line(index);
}

void MainWindow::markerSignal()
{
    controlMessage_Marker();
}

bool MainWindow::jump_to_line(int line)
{

    int row = nearest_line(line);
    if (0 > row)
        return false;

    ui->tableView->selectionModel()->clear();
    QModelIndex idx = tableModel->index(row, 0, QModelIndex());
    ui->tableView->scrollTo(idx, QAbstractItemView::PositionAtTop);
    ui->tableView->selectionModel()->select(idx, QItemSelectionModel::Select|QItemSelectionModel::Rows);
    ui->tableView->setFocus();

    return true;
}

void MainWindow::on_actionJump_To_triggered()
{
    JumpToDialog dlg(this);
    int min = 0;
    int max = qfile.size()-1;
    dlg.setLimits(min, max);

    int result = dlg.exec();

    if(result != QDialog::Accepted)
    {
        return;
    }

    jump_to_line(dlg.getIndex());


}

void MainWindow::on_actionAutoScroll_triggered(bool checked)
{
    int autoScrollOld = settings->autoScroll;

    // Mapping: button to variable
    settings->autoScroll = (checked?Qt::Checked:Qt::Unchecked);

    if (autoScrollOld!=settings->autoScroll)
        settings->writeSettings(this);

    // inform plugins about changed autoscroll status
    pluginManager.autoscrollStateChanged(settings->autoScroll);
}

void MainWindow::on_actionConnectAll_triggered()
{
    connectAll();
}

void MainWindow::on_actionDisconnectAll_triggered()
{
    disconnectAll();
}

void MainWindow::on_pluginsEnabled_clicked(bool checked)
{
    DltSettingsManager::getInstance()->setValue("startup/pluginsEnabled", checked);
    applyConfigEnabled(true);
}

void MainWindow::on_filtersEnabled_clicked(bool checked)
{
    DltSettingsManager::getInstance()->setValue("startup/filtersEnabled", checked);
    ui->checkBoxSortByTime->setEnabled(checked);
    applyConfigEnabled(true);
}

void MainWindow::on_checkBoxSortByTime_clicked(bool checked)
{
    DltSettingsManager::getInstance()->setValue("startup/sortByTimeEnabled", checked);
    applyConfigEnabled(true);
}

void MainWindow::on_applyConfig_clicked()
{
    applyConfigEnabled(false);
    filterUpdate();
    reloadLogFile(true);
}

void MainWindow::clearSelection()
{
    previousSelection.clear();
    ui->tableView->selectionModel()->clear();
}

void MainWindow::saveSelection()
{
    previousSelection.clear();
    /* Store old selections */
    QModelIndexList rows = ui->tableView->selectionModel()->selectedRows();

    for(int i=0;i<rows.count();i++)
    {
        int sr = rows.at(i).row();
        previousSelection.append(qfile.getMsgFilterPos(sr));
        qDebug() << "Save Selection " << i << " at line " << qfile.getMsgFilterPos(sr);
    }
}

void MainWindow::restoreSelection()
{
    int firstIndex = 0;
    //QModelIndex scrollToTarget = tableModel->index(0, 0);
    QItemSelection newSelection;

    // clear current selection model
    ui->tableView->selectionModel()->clear();

    // check if anything was selected
    if(previousSelection.count()==0)
        return;

    // restore all selected lines
    for(int j=0;j<previousSelection.count();j++)
    {
        int nearestIndex = nearest_line(previousSelection.at(j));

        qDebug() << "Restore Selection" << j << "at index" << nearestIndex << "at line" << previousSelection.at(0);

        if(j==0)
        {
            firstIndex = nearestIndex;
        }

        QModelIndex idx = tableModel->index(nearestIndex, 0);
        newSelection.select(idx, idx);
    }

    // set all selections
    ui->tableView->selectionModel()->select(newSelection, QItemSelectionModel::Select|QItemSelectionModel::Rows);

    // scroll to first selected row
    ui->tableView->setFocus();  // focus must be set before scrollto is possible
    QModelIndex idx = tableModel->index(firstIndex, 0, QModelIndex());
    ui->tableView->scrollTo(idx, QAbstractItemView::PositionAtTop);
}

void MainWindow::on_tabWidget_currentChanged(int index)
{
    if(index > 0)
    {
        ui->enableConfigFrame->setVisible(true);
    }
    else
    {
        ui->enableConfigFrame->setVisible(false);
    }
}

void MainWindow::filterOrderChanged()
{
    filterUpdate();
    tableModel->modelChanged();
}

void MainWindow::searchTableRenewed()
{
    if ( 0 < m_searchtableModel->get_SearchResultListSize())
        ui->dockWidgetSearchIndex->show();

    m_searchtableModel->modelChanged(); 
}


void MainWindow::searchtable_cellSelected( QModelIndex index)
{

    int position = index.row();
    unsigned long entry;

    if (! m_searchtableModel->get_SearchResultEntry(position, entry) )
        return;

    tableModel->setLastSearchIndex(entry);
    jump_to_line(entry);

}

void MainWindow::on_comboBoxFilterSelection_activated(const QString &arg1)
{
    /* check if not "no default filter" item selected */
    if(ui->comboBoxFilterSelection->currentIndex()==0)
    {
        /* reset all default filter index */
        defaultFilter.clearFilterIndex();

        return;
    }

    /* load current selected filter */
    if(!arg1.isEmpty() && project.LoadFilter(arg1,true))
    {
        workingDirectory.setDlfDirectory(QFileInfo(arg1).absolutePath());
        setCurrentFilters(arg1);

        /* if filter index already stored default filter cache, use index from cache */
        QDltFilterIndex *index = defaultFilter.defaultFilterIndex[ui->comboBoxFilterSelection->currentIndex()-1];

        /* check if filename and qfile size is matching cache entry */
        if(index->allIndexSize == qfile.size() &&
           index->dltFileName == qfile.getFileName())
        {
            /* save selection */
            saveSelection();

            /* filter index cache found */
            /* copy index into file */
            qfile.setIndexFilter(index->indexFilter);

            /* update ui */
            applyConfigEnabled(false);
            filterUpdate();
            tableModel->modelChanged();
            m_searchtableModel->modelChanged();
            restoreSelection();
        }
        else
        {
            /* filter index cache not found */
            /* Activate filter and create index there as usual */
            on_applyConfig_clicked();

            /* Now store the created index in the default filter cache */
            QDltFilterIndex *index = defaultFilter.defaultFilterIndex[ui->comboBoxFilterSelection->currentIndex()-1];
            index->setIndexFilter(qfile.getIndexFilter());
            index->setDltFileName(qfile.getFileName());
            index->setAllIndexSize(qfile.size());
        }
        ui->tabWidget->setCurrentWidget(ui->tabPFilter);
        on_filterWidget_itemSelectionChanged();
    }
}

void MainWindow::on_actionDefault_Filter_Reload_triggered()
{
    QDir dir;

    /* clear combobox default filter */
    ui->comboBoxFilterSelection->clear();

    /* add "no default filter" entry */
    ui->comboBoxFilterSelection->addItem("<No filter selected>");

    /* clear default filter list */
    defaultFilter.clear();

    // check if default filter enabled
    if(!settings->defaultFilterPath)
    {
        return;
    }

    /* get the filter path */
    dir.setPath(settings->defaultFilterPathName);

    /* update tooltip */
    ui->comboBoxFilterSelection->setToolTip(QString("Multifilterlist in folder %1").arg(dir.absolutePath()));

    /* check if directory for configuration exists */
    if(!dir.exists())
    {
        /* directory does not exist, make it */
        if(!dir.mkpath(dir.absolutePath()))
        {
            /* creation of directory fails */
            QMessageBox::critical(0, QString("DLT Viewer"),
                                           QString("Cannot create directory to store cache files!\n\n")+dir.absolutePath(),
                                           QMessageBox::Ok,
                                           QMessageBox::Ok);
            return;
        }
    }

    /* load the default filter list */
    defaultFilter.load(dir.absolutePath());

    /* default filter list update combobox */
    QDltFilterList *filterList;
    foreach(filterList,defaultFilter.defaultFilterList)
        ui->comboBoxFilterSelection->addItem(filterList->getFilename());

}

void MainWindow::on_actionDefault_Filter_Create_Index_triggered()
{
    /* reset default filter list and reload from directory all default filter */
    reloadLogFileDefaultFilter();
}

void MainWindow::applyConfigEnabled(bool enabled)
{
    if(enabled)
    {
        /* show apply config button */
        ui->applyConfig->startPulsing(pulseButtonColor);
        ui->applyConfig->setEnabled(true);

        /* reset default filter selection and default filter index */
        resetDefaultFilter();
    }
    else
    {
        /* hide apply config button */
        ui->applyConfig->stopPulsing();
        ui->applyConfig->setEnabled(false);
    }
}

void MainWindow::resetDefaultFilter()
{
    /* reset all default filter index */
    defaultFilter.clearFilterIndex();

    /* select "no default filter" entry */
    ui->comboBoxFilterSelection->setCurrentIndex(0); //no default filter anymore
}

void MainWindow::on_pushButtonDefaultFilterUpdateCache_clicked()
{
    on_actionDefault_Filter_Create_Index_triggered();
}

void MainWindow::on_actionMarker_triggered()
{
    controlMessage_Marker();
}
