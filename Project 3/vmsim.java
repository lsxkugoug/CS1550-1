import java.io.File;
import java.io.FileNotFoundException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.Hashtable;
import java.util.LinkedList;
import java.util.Scanner;

public class vmsim {

	static String alg 	= "LRU"; 	// default value in case no input args
	static String file 	= "swim.trace"; // default value in case no input args
	static int mem_accs = 0, 
			page_faults = 0, 
			disk_writes = 0, 
			numframes   = 0;
	static int PAGE_SIZE_BITS = 12; 	// Page size is 4KB = 2^12
	static ArrayList<Instruction> I; 	// used to store each line of trace file as new data structure
	static Hashtable<Long, Page> framez; // stores pages currently in RAM for all algs
	static Hashtable<Long, Page> pagez;	 // stores all pages that are accessed
	static long start;	// used to store start time, for testing purposes
	static int SCAi = 0;// "clock hand" pointer, short for Second Chance Algorithm Index
	
	static Page [] SRAM; // data structure used to keep track of RAM in Second Chance
	static LinkedList<Long> LRAM; // data structure used to keep track of RAM in LRU
	
	public static void main(String args[]) {
		
//		start = System.nanoTime(); 	// for testing
		get_cli_args(args);			// parse command line args or use default values if there are none
		get_trace_data(); 			// get trace data from file and load into I and pagez
		go();						// run test with given alg/tracefile/frames
		show_results();				// print stats of test
//		show_time_elapsed(); 		// for testing
		
	}

	private static void go() {
		
		Instruction i;
		mem_accs = I.size(); // trace file is just a list of all mem accs, so the size of the instruction list will be equal to # of mem accs
		
		for (int x = 0; x < I.size(); x++) { // loop through all instructions
			
			i = I.get(x); // get current instruction, holds page # and either "l" or "s"
			
			if (!framez.containsKey(i.page)) {
				
				page_faults++; // page not in RAM means page fault
				
				if (!add_page_successful(i.page)) { // if page not able to be added because RAM is full, we need to evict
					
					Long old_page = null;
					Long new_page = i.page;
					
					switch (alg) { // use algorithm based on input arg
					
						case "OPT":
							old_page = OPT(i.page, x);
							break;
						case "LRU": 
							old_page = LRU(i.page);
							break;
						case "SECOND":
							old_page = SCA(i.page);
							break;
						default:
							System.out.println("Invalid algorithm.");
							System.exit(0);
						
					}
					
					replace_page(old_page, new_page); // swap evicted page for incoming page

				}
			
			} else {
				if (alg.equalsIgnoreCase("SECOND")) { set_ref(i.page, 1); } // in Second Chance we need to set R=1 every time page in RAM is accessed
				if (alg.equalsIgnoreCase("LRU")) { // in LRU we need to move page to the back of the LinkedList if accessed
					LRAM.remove(i.page);
					LRAM.addLast(i.page);
				}
			}
			
			if (i.action.equalsIgnoreCase("s")) { set_dirty(i.page, 1); } // if page is altered we will need to write it back to disk when evicted
			
		}
		
	}

	private static Long OPT(long p, int z) {
		
		int [] index = new int[numframes];
		int c = 0;
		
		for (Enumeration<Long> key = framez.keys(); key.hasMoreElements();) { // loop through pages currently in RAM
			
			index[c] = -1;
			long k = framez.get(key.nextElement()).n; // get page address
			
			for (int i = z; i < I.size(); i++) { // start at current instruction and look for next time page is accessed
				if (I.get(i).page == k) {
					index[c] = i; 
					break; // stop looping once we find next occurrence
				}
			}
			
			if (index[c] == -1) { return k; } // if page never accessed again, evict it
			c++;
			
		}
		
		int max = 0;
		for (int i = 0; i < index.length; i++) {
			if (index[i] > max) { max = index[i]; }
		} // max will be representative of the furthest away a current page in memory will be accessed - that will be the page we want to evict
		
		return I.get(max).page;  // return evicted page to be swapped in framez
		
	}

	private static Long SCA(long p) {
		
		Long ret = null;
		
		for (int i = 0; i < numframes; i++) { // loop through pages currently in RAM
			
			if (SRAM[SCAi].ref == 0) { 
				break; // page already had second chance, evict it
			} else { 
				set_ref(SRAM[SCAi].n, 0); 		// give page second chance
				SCAi = (SCAi + 1) % numframes; 	// move to next spot on "clock"
			}
			
		}
		
		ret = SRAM[SCAi].n; 		// return only page address, not Page object
		SRAM[SCAi] = pagez.get(p); 	// replace evicted page with incoming page
		set_ref(SRAM[SCAi].n, 0); 	// new page should start with R=0
		SCAi = (SCAi+1) % numframes;// move to next spot on "clock"
		return ret;  				// return evicted page to be swapped in framez
		
	}
	
	private static Long LRU(long page) {
		
		long ret = LRAM.pollFirst(); // head of linked list will be least recently accessed page
		LRAM.addLast(page);			 // add incoming page to end of linked list, as it is the most recently used
		return ret;					 // return evicted page to be swapped in framez
		
	}
	
	private static void set_dirty(long p, int b) {
		 pagez.get(p).dirty = b; 
		framez.get(p).dirty = b;
	}
	
	private static void set_ref(long p, int b) {
		
		if (alg.equalsIgnoreCase("SECOND")) {
			for (int i = 0; i < numframes; i++) { // find correct page in RAM to set R=b
				if (SRAM[i].n == p) {
					SRAM[i].ref = b;
					break; // dont need to finish loop once we find page
				}
			}
		}
		
		pagez.get(p).ref  = b;
		if (framez.get(p) != null) { framez.get(p).ref = b; }
		
	}
		
	private static void replace_page(Long old_page, Long new_page) {
		
		disk_writes += framez.get(old_page).dirty; // if page was dirty, we need to write it back to disk
		set_dirty(old_page, 0);	// set dirty bit back to 0
		framez.remove(old_page); 
		framez.put(new_page, pagez.get(new_page));
		
	}

	private static boolean add_page_successful(long p) {
		
		if (framez.size() < numframes) { // if RAM isnt full, then add to RAM
			framez.put(p, pagez.get(p));
			
			if (alg.equals("SECOND")) { // add to Second Chance helper data structure if need be
				SRAM[SCAi] = pagez.get(p);
				SCAi = (SCAi + 1) % numframes;
			}
			
			if (alg.equalsIgnoreCase("LRU")) { // add to LRU helper data structure if need be
				LRAM.addLast(p);
			}
			
			return true;
		} else {
			return false;
		}
	
	}

	private static void show_results() {
		System.out.println("Algorithm: " 			 + alg.toUpperCase());
		System.out.println("Number of frames: " 	 + numframes);
		System.out.println("Total memory accesses: " + mem_accs);
		System.out.println("Total page faults: " 	 + page_faults);
		System.out.println("Total writes to disk: "  + disk_writes);
	}
	
	private static void show_time_elapsed() { // shows difference in time, for testing
		System.out.println("Time: " + ((System.nanoTime() - start) / 1000000000.0)); 
	}

	private static void get_trace_data() {
		
		Scanner fStream = null;
		String s = null;
		long   n = 0;
		try { fStream = new Scanner(new File(file)); } 
		catch (FileNotFoundException e) { System.out.println("Trace file not found, exiting."); System.exit(0); }
		
		while (fStream.hasNext()) {
			s = fStream.next(); // stores either "s" or "l"
			n = Long.parseLong(fStream.next().substring(2), 16) >> PAGE_SIZE_BITS; // dont need exact address, the page it is contained in will do
			I.add(new Instruction(s, n));
			if (!pagez.containsKey(n)) { pagez.put(n, new Page(n)); } // if new page, add it to hash list
		}
		
		System.out.println(pagez.size());
		
	}

	private static void get_cli_args(String[] args) { // parse command line arguments 
		
		for (int i = 0; i < args.length; i++) {			

			if (args[i].equals("-n")) {
				
				numframes = Integer.parseInt(args[i+1]);
				
			} else if (args[i].equals("-a")) {
				
				alg = args[i+1].toUpperCase();
				if (alg.equals("SCA")) { alg = "SECOND"; } 
				
				boolean valid = alg.equalsIgnoreCase("OPT")  ||
							    alg.equalsIgnoreCase("LRU")  ||
							    alg.equalsIgnoreCase("SECOND");
				
				if (!valid) { System.exit(0); }
						
			}
			
		}
		
		if (args.length != 0) { 
			file = args[args.length -1]; 
		} else {
			if   (alg.equalsIgnoreCase("SECOND") && !file.equalsIgnoreCase("simple.trace"))  { numframes = 8; }
			else if (alg.equalsIgnoreCase("LRU") &&  file.equalsIgnoreCase("simple2.trace")){ numframes = 3; } 
			else 																		   { numframes = 2; }
		}
		
		I 	   = new ArrayList<Instruction>();
		framez = new Hashtable<Long, Page>((int)(numframes));
		 pagez = new Hashtable<Long, Page>();
		
		if (alg.equalsIgnoreCase("SECOND")) { SRAM = new Page[numframes]; }
		if (alg.equalsIgnoreCase("LRU"))    { LRAM = new LinkedList<Long>(); }
		
		
	}

}

class Instruction {
	
	public String   action; // "l" or "s"
	public long 	page;	// address >> 12
	
	Instruction(String s, long i) {
		this.action = s;
		this.page	= i;
	}

}

class Page {
	
	public long n;
	public int 	ref   = 0;
	public int  dirty = 0;
	
	Page(Page p) {
		this.n 		= p.n;
		this.ref 	= p.ref;
		this.dirty 	= p.dirty;
	}
	
	Page(long i) {
		this.n 		= i;		
	}
	
}
